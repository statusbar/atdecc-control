// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nadia_proxy/nadia_proxy.hpp"

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/controlplane_devices/nadia_ascii.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace statusbar::nadia_proxy {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using controlplane::AemRef;
using controlplane::ControlPointId;
using controlplane_devices::NadiaNamespace;
using controlplane_devices::NadiaValueKind;
using ieee::Eui64;

namespace {

/// Tag on the liveness `get`; the Core echoes it as "Got (liveness): ...",
/// which poll() recognises and drops (NadiaProxyConfig::southbound_quiet_ms).
constexpr std::string_view LIVENESS_TAG = "liveness";
constexpr std::string_view LIVENESS_REPLY_PREFIX = "Got (liveness)";

constexpr std::uint16_t IDENTIFY_CONTROL = ProxyHandler::IDENTIFY_CONTROL;

Eui64 eui64_from(std::uint64_t v)
{
    return Eui64{
        std::uint8_t(v >> 56),
        std::uint8_t(v >> 48),
        std::uint8_t(v >> 40),
        std::uint8_t(v >> 32),
        std::uint8_t(v >> 24),
        std::uint8_t(v >> 16),
        std::uint8_t(v >> 8),
        std::uint8_t(v)};
}

/// The proxy's descriptor handler over the generated blob: identity patched
/// in, SET_NAME stickers managed in RAM (localized strings keep the identity).
ProxyHandler make_proxy_handler(NadiaAemModel const& aem, NadiaProxyConfig const& config)
{
    auto storage = DescriptorStorage::create(aem.blob);
    ProxyHandler handler{*storage, eui64_from(derive_proxy_entity_id(config.nadia_entity_id)), eui64_from(config.nadia_entity_id)};
    handler.manage_object_names();
    return handler;
}

bool is_float_kind(NadiaValueKind kind)
{
    return kind == NadiaValueKind::decibels || kind == NadiaValueKind::hertz || kind == NadiaValueKind::ratio;
}

/// The text a set carries: float kinds at single precision (the served
/// value is a float, and 10 significant digits of a widened float would
/// send "-3.599999904" for -3.6).
std::string format_for_wire(NadiaValueKind kind, double value)
{
    if (is_float_kind(kind)) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.7g", value);
        return buf;
    }
    return controlplane_devices::nadia_format_value(kind, value);
}

/// Same value as far as the served format can tell.
bool same_value(NadiaValueKind kind, double mirrored, double requested)
{
    if (is_float_kind(kind)) {
        return float(mirrored) == float(requested);
    }
    return mirrored == requested;
}

double decode_element(std::uint16_t value_type, std::span<std::uint8_t const> bytes)
{
    switch (value_type) {
        case CONTROL_LINEAR_FLOAT:
        case CONTROL_ARRAY_FLOAT: {
            std::array<std::uint8_t, 4> b{};
            std::memcpy(b.data(), bytes.data(), 4);
            return double(detail::decode_linear_field<float>(b));
        }
        case CONTROL_LINEAR_INT32:
        case CONTROL_ARRAY_INT32: {
            std::array<std::uint8_t, 4> b{};
            std::memcpy(b.data(), bytes.data(), 4);
            return double(detail::decode_linear_field<std::int32_t>(b));
        }
        case CONTROL_LINEAR_INT64:
        case CONTROL_ARRAY_INT64: {
            std::array<std::uint8_t, 8> b{};
            std::memcpy(b.data(), bytes.data(), 8);
            return double(detail::decode_linear_field<std::int64_t>(b));
        }
        default:
            return double(bytes[0]);
    }
}

void encode_element(std::uint16_t value_type, double value, std::span<std::uint8_t> out)
{
    switch (value_type) {
        case CONTROL_LINEAR_FLOAT:
        case CONTROL_ARRAY_FLOAT: {
            std::array<std::uint8_t, 4> b{};
            detail::encode_linear_field(float(value), b);
            std::memcpy(out.data(), b.data(), 4);
            return;
        }
        case CONTROL_LINEAR_INT32:
        case CONTROL_ARRAY_INT32: {
            std::array<std::uint8_t, 4> b{};
            detail::encode_linear_field(std::int32_t(value), b);
            std::memcpy(out.data(), b.data(), 4);
            return;
        }
        case CONTROL_LINEAR_INT64:
        case CONTROL_ARRAY_INT64: {
            std::array<std::uint8_t, 8> b{};
            detail::encode_linear_field(std::int64_t(std::llround(value)), b);
            std::memcpy(out.data(), b.data(), 8);
            return;
        }
        default:
            out[0] = std::uint8_t(value);
            return;
    }
}

/// A boolean's AEM representation is 0/255; the mirror's is 0/1.
double schema_to_wire(NadiaValueKind kind, double schema)
{
    return kind == NadiaValueKind::boolean ? (schema != 0.0 ? 255.0 : 0.0) : schema;
}

double wire_to_schema(NadiaValueKind kind, double wire)
{
    return kind == NadiaValueKind::boolean ? (wire != 0.0 ? 1.0 : 0.0) : wire;
}

}  // namespace

struct NadiaProxy::Boot
{
    NadiaNamespace ns;
    NadiaAemModel aem;
};

NadiaProxy::Boot* NadiaProxy::make_boot(controlplane_devices::NadiaShape const& shape)
{
    auto ns = controlplane_devices::build_nadia_namespace(shape);
    auto aem = build_nadia_proxy_aem_model(ns);
    return new Boot{std::move(ns), std::move(aem)};
}

NadiaProxy::NadiaProxy(NadiaProxyConfig config)
    : NadiaProxy(std::move(config), *std::unique_ptr<Boot>(make_boot(config.shape)))
{}

NadiaProxy::NadiaProxy(NadiaProxyConfig config, Boot boot)
    : config_(std::move(config))
    , aem_(std::move(boot.aem))
    , mirror_(std::move(boot.ns))
    , handler_(make_proxy_handler(aem_, config_))
    , host_(
          handler_,
          nanoavb::AdpAdvertiserConfig{},
          /*talker_max_streams=*/0,
          /*talker_max_listeners=*/1,
          /*listener_max_streams=*/0)
{
    // Per-control wire-format table from the generated descriptors.
    for (auto const& d : aem_.descriptors) {
        if (d.descriptor_type != DESCRIPTOR_CONTROL) {
            continue;
        }
        DescriptorControl c;
        span_load_padded(c, d.bytes);
        auto bits = unpack_control_value_type(std::uint16_t(c.control_value_type.get()));
        if (control_info_.size() <= d.descriptor_index) {
            control_info_.resize(d.descriptor_index + 1u);
        }
        control_info_[d.descriptor_index] = {
            bits.value_type,
            std::uint16_t(c.number_of_values.get()),
            bits.value_type == CONTROL_UTF8 ? std::size_t(0) : control_value_element_size(bits.value_type),
            bits.read_only,
            bits.value_type == CONTROL_UTF8,
        };
    }
    dirty_controls_.assign(control_info_.size(), false);

    handler_.set_hooks(
        [this](std::uint16_t idx, std::span<std::uint8_t> out) -> std::size_t {
            auto bytes = encode_control(idx);
            if (bytes.empty() || out.size() < bytes.size()) {
                return 0;
            }
            std::memcpy(out.data(), bytes.data(), bytes.size());
            return bytes.size();
        },
        [this](std::uint16_t idx, std::span<std::uint8_t const> values) { return on_northbound_control(idx, values); },
        [this](std::uint16_t idx, DescriptorControl& desc) {
            if (idx >= control_info_.size() || control_info_[idx].utf8) {
                return;
            }
            auto const& info = control_info_[idx];
            auto const bytes = encode_control(idx);
            auto trailer = desc.value_details_bytes_mutable();
            bool const array = is_array_value_type(info.value_type);
            for (std::uint16_t v = 0; v < info.count; ++v) {
                std::size_t const at = array
                    ? 4 * info.element_size + 4 + std::size_t(v) * info.element_size
                    : std::size_t(v) * linear_entry_size(info.element_size) + linear_entry_current_offset(info.element_size);
                if (at + info.element_size > trailer.size()) {
                    break;
                }
                std::memcpy(trailer.data() + at, bytes.data() + std::size_t(v) * info.element_size, info.element_size);
            }
        });
    liveness_ping_ = controlplane_devices::nadia_get("system level", LIVENESS_TAG);
}

Eui64 NadiaProxy::proxy_entity_id() const noexcept
{
    return eui64_from(derive_proxy_entity_id(config_.nadia_entity_id));
}

bool NadiaProxy::connect_southbound()
{
    using namespace controlplane_devices;
    if (!client_.connect(config_.nadia_host, config_.nadia_port)) {
        return false;
    }
    for (auto const& pattern : mirror_.ns().patterns) {
        if (!client_.send(nadia_get(pattern.addresses))) {
            return false;
        }
    }
    for (auto const& pattern : mirror_.ns().patterns) {
        if (!client_.send(nadia_subscribe(pattern.addresses))) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> NadiaProxy::encode_control(std::uint16_t control_index) const
{
    std::vector<std::uint8_t> out;
    if (control_index >= control_info_.size()) {
        return out;
    }
    auto const& info = control_info_[control_index];
    auto const& ns = mirror_.ns();

    if (info.utf8) {
        auto bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_CONTROL, control_index, 0});
        if (bound && mirror_.populated().test(*bound)) {
            auto const& text = mirror_.value_of(*bound);
            auto n = std::min<std::size_t>(text.size(), DescriptorControl::MAX_VALUE_DETAILS - 1);
            out.assign(text.begin(), text.begin() + long(n));
        }
        out.push_back(0);
        return out;
    }

    // Numeric: the mirrored scalar, else the authored default (the
    // descriptor's CURRENT field, which the generator set to the default).
    out.resize(std::size_t(info.count) * info.element_size);
    for (std::uint16_t v = 0; v < info.count; ++v) {
        auto slot = std::span(out).subspan(std::size_t(v) * info.element_size, info.element_size);
        auto bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_CONTROL, control_index, v});
        std::optional<double> scalar = bound ? mirror_.scalar_of(*bound) : std::nullopt;
        if (scalar) {
            encode_element(info.value_type, schema_to_wire(ns.kinds[*bound], *scalar), slot);
            continue;
        }
        bool const array = is_array_value_type(info.value_type);
        std::size_t const at = DescriptorControl::LENGTH +
            (array ? 4 * info.element_size + 4 + std::size_t(v) * info.element_size
                   : std::size_t(v) * linear_entry_size(info.element_size) + linear_entry_current_offset(info.element_size));
        for (auto const& d : aem_.descriptors) {
            if (d.descriptor_type == DESCRIPTOR_CONTROL && d.descriptor_index == control_index &&
                at + info.element_size <= d.bytes.size()) {
                std::memcpy(slot.data(), d.bytes.data() + at, info.element_size);
                break;
            }
        }
    }
    return out;
}

bool NadiaProxy::send_southbound_set(ControlPointId h, double value)
{
    // Echo suppression: a value already at the mirrored state needs no set;
    // this is also what terminates the set -> report -> notify loop, and
    // what makes write-current-value-back a safe northbound exercise.
    auto const kind = mirror_.ns().kinds[h];
    if (auto mirrored = mirror_.scalar_of(h); mirrored && same_value(kind, *mirrored, value)) {
        return true;
    }
    auto const& path = mirror_.ns().adaptor.address_of(h);
    auto formatted = format_for_wire(kind, value);
    if (!client_.send(controlplane_devices::nadia_set(controlplane_devices::nadia_command_address(path), formatted))) {
        return false;
    }
    // Optimistic local echo (the GALAXY did not push every change made
    // over its own connection back; assume the same here until the Core
    // proves otherwise), so a stale mirrored value never suppresses the
    // set that undoes a previous one. A later device report overwrites
    // this if the device disagrees.
    mirror_.record_local_set(
        h,
        formatted,
        std::uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    ++southbound_sets_;
    return true;
}

bool NadiaProxy::send_southbound_text(ControlPointId h, std::string_view text)
{
    if (mirror_.populated().test(h) && mirror_.value_of(h) == text) {
        return true;
    }
    auto const& path = mirror_.ns().adaptor.address_of(h);
    if (!client_.send(controlplane_devices::nadia_set(controlplane_devices::nadia_command_address(path), text))) {
        return false;
    }
    mirror_.record_local_set(
        h,
        text,
        std::uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    ++southbound_sets_;
    return true;
}

std::uint8_t NadiaProxy::on_northbound_control(std::uint16_t control_index, std::span<std::uint8_t const> values)
{
    if (control_index >= control_info_.size()) {
        return AEM_STATUS_NO_SUCH_DESCRIPTOR;
    }
    auto const& info = control_info_[control_index];
    if (info.read_only) {
        return AEM_STATUS_NOT_SUPPORTED;
    }
    auto const& ns = mirror_.ns();

    if (info.utf8) {
        auto bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_CONTROL, control_index, 0});
        if (!bound) {
            return AEM_STATUS_NOT_SUPPORTED;
        }
        std::string_view text(reinterpret_cast<char const*>(values.data()), values.size());
        if (auto nul = text.find('\0'); nul != std::string_view::npos) {
            text = text.substr(0, nul);
        }
        if (text.find('\n') != std::string_view::npos || text.find('\r') != std::string_view::npos) {
            return AEM_STATUS_BAD_ARGUMENTS;  // a line break would split the command
        }
        return send_southbound_text(*bound, text) ? AEM_STATUS_SUCCESS : AEM_STATUS_NOT_SUPPORTED;
    }

    if (values.size() < std::size_t(info.count) * info.element_size) {
        return AEM_STATUS_BAD_ARGUMENTS;
    }
    for (std::uint16_t v = 0; v < info.count; ++v) {
        auto bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_CONTROL, control_index, v});
        if (!bound) {
            continue;
        }
        auto wire = decode_element(info.value_type, values.subspan(std::size_t(v) * info.element_size, info.element_size));
        auto const schema = wire_to_schema(ns.kinds[*bound], wire);
        auto const& m = *ns.model;
        if (m.min(*bound) < m.max(*bound) && (schema < m.min(*bound) || schema > m.max(*bound))) {
            return AEM_STATUS_BAD_ARGUMENTS;
        }
        if (!send_southbound_set(*bound, schema)) {
            return AEM_STATUS_NOT_SUPPORTED;
        }
    }
    // Other controllers learn of the change from the device's report; a
    // Core that does not echo its own connection's sets would leave them
    // stale, so fan out the optimistic mirror state now as well.
    if (!dirty_controls_[control_index]) {
        dirty_controls_[control_index] = true;
        dirty_list_.push_back(control_index);
    }
    return AEM_STATUS_SUCCESS;
}

void NadiaProxy::notify_control(std::uint16_t control_index)
{
    if (!running()) {
        return;
    }
    auto values = encode_control(control_index);
    std::vector<std::uint8_t> body;
    body.reserve(4 + values.size());
    body.push_back(std::uint8_t(DESCRIPTOR_CONTROL >> 8));
    body.push_back(std::uint8_t(DESCRIPTOR_CONTROL & 0xFF));
    body.push_back(std::uint8_t(control_index >> 8));
    body.push_back(std::uint8_t(control_index & 0xFF));
    body.insert(body.end(), values.begin(), values.end());
    host_.components().aem_handler.emit_unsolicited(AEM_COMMAND_SET_CONTROL, body);
    ++unsolicited_;
}

int NadiaProxy::poll(unsigned timeout_ms, std::uint64_t now_ns)
{
    int lines = client_.poll_lines(timeout_ms, [&](std::string_view line) {
        if (line.starts_with(LIVENESS_REPLY_PREFIX)) {
            return;  // the ping's reply: proof of life, not a change
        }
        auto h = mirror_.consume_line(line, now_ns);
        if (!h) {
            return;
        }
        auto const& ref = aem_.binding.address_of(*h);
        if (ref.descriptor_type == std::uint16_t(DESCRIPTOR_CONTROL) && ref.descriptor_index < dirty_controls_.size() &&
            !dirty_controls_[ref.descriptor_index]) {
            dirty_controls_[ref.descriptor_index] = true;
            dirty_list_.push_back(ref.descriptor_index);
        }
    });
    if (lines < 0) {
        return lines;
    }
    if (client_.check_liveness(
            now_ns,
            std::uint64_t(config_.southbound_quiet_ms) * 1'000'000u,
            std::uint64_t(config_.southbound_ping_timeout_ms) * 1'000'000u,
            liveness_ping_) < 0) {
        return -1;
    }
    for (auto idx : dirty_list_) {
        dirty_controls_[idx] = false;
        notify_control(idx);
    }
    dirty_list_.clear();
    return lines;
}

Status NadiaProxy::start(net::MessageReactor& reactor, std::string_view interface_name)
{
    if (config_.mirror_avb_info) {
        host_.mirror_avb_info_from(
            eui64_from(config_.nadia_entity_id), eui64_from(derive_proxy_entity_id(config_.nadia_entity_id)));
    }
    auto st = host_.start_control_plane(reactor, interface_name);
    if (st) {
        // Drive the supervisor out of Down: link up now; poll() then feeds
        // start_protocols on that transition is what starts ADP. (Init ->
        // Ready needs gPTP lock, but ADP does not; Init's Timeout event is
        // the gPTP watchdog back to Down — never feed it from here.)
        host_.on_link_up(sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()});
    }
    return st;
}

Status NadiaProxy::stop()
{
    return host_.stop_control_plane(sm::TimePoint{});
}

}  // namespace statusbar::nadia_proxy
