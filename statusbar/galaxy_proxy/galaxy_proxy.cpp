// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/galaxy_proxy/galaxy_proxy.hpp"

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/controlplane_devices/galaxy_ascii.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace statusbar::galaxy_proxy {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using controlplane::AemRef;
using controlplane::ControlPointId;
using controlplane_devices::GalaxyNamespace;
using ieee::Eui64;

namespace {

constexpr std::uint16_t MATRIX_WIDTH = 16;

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
ProxyHandler make_proxy_handler(GalaxyAemModel const& aem, GalaxyProxyConfig const& config)
{
    auto storage = DescriptorStorage::create(aem.blob);
    ProxyHandler handler{
        *storage, eui64_from(derive_proxy_entity_id(config.galaxy_entity_id)), eui64_from(config.galaxy_entity_id)};
    handler.manage_object_names();
    return handler;
}

/// Is this schema point boolean-valued on the wire (true/false strings)?
bool is_boolean_point(GalaxyNamespace const& ns, ControlPointId h)
{
    auto const& def = ns.defaults[h];
    return def == "true" || def == "false";
}

/// Formats a numeric value the way the GALAXY ASCII protocol expects.
std::string format_value(GalaxyNamespace const& ns, ControlPointId h, double value)
{
    if (is_boolean_point(ns, h)) {
        return value != 0.0 ? "true" : "false";
    }
    char buf[48];
    snprintf(buf, sizeof buf, "%g", value);
    return buf;
}

/// The point's current numeric value as mirrored (schema default until the
/// device reports).
double mirrored_value(controlplane_devices::GalaxyMirror const& mirror, ControlPointId h)
{
    auto const& s = mirror.reported().test(h) ? mirror.value_of(h) : mirror.ns().defaults[h];
    if (s == "true") {
        return 1.0;
    }
    if (s == "false") {
        return 0.0;
    }
    return std::strtod(s.c_str(), nullptr);
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
        case CONTROL_LINEAR_INT16: {
            std::array<std::uint8_t, 2> b{};
            std::memcpy(b.data(), bytes.data(), 2);
            return double(detail::decode_linear_field<std::int16_t>(b));
        }
        case CONTROL_LINEAR_UINT8:
        default:
            return double(bytes[0]);
    }
}

void encode_element(std::uint16_t value_type, double value, std::vector<std::uint8_t>& out)
{
    switch (value_type) {
        case CONTROL_LINEAR_FLOAT:
        case CONTROL_ARRAY_FLOAT: {
            std::array<std::uint8_t, 4> b{};
            detail::encode_linear_field(float(value), b);
            out.insert(out.end(), b.begin(), b.end());
            return;
        }
        case CONTROL_LINEAR_INT16: {
            std::array<std::uint8_t, 2> b{};
            detail::encode_linear_field(std::int16_t(value), b);
            out.insert(out.end(), b.begin(), b.end());
            return;
        }
        case CONTROL_LINEAR_UINT8:
        default:
            out.push_back(std::uint8_t(value));
            return;
    }
}

/// A boolean's AEM representation is 0/255; the GALAXY's is true/false.
double wire_to_schema_value(GalaxyNamespace const& ns, ControlPointId h, double wire)
{
    if (is_boolean_point(ns, h)) {
        return wire != 0.0 ? 1.0 : 0.0;
    }
    return wire;
}

double schema_to_wire_value(GalaxyNamespace const& ns, ControlPointId h, double schema)
{
    if (is_boolean_point(ns, h)) {
        return schema != 0.0 ? 255.0 : 0.0;
    }
    return schema;
}

}  // namespace

struct GalaxyProxy::Boot
{
    GalaxyNamespace ns;
    GalaxyAemModel aem;
};

GalaxyProxy::Boot* GalaxyProxy::make_boot(std::string_view model_string)
{
    auto ns = controlplane_devices::build_galaxy_816_namespace();
    auto aem = build_galaxy_proxy_aem_model(ns, galaxy_model_banks(model_string));
    return new Boot{std::move(ns), std::move(aem)};
}

GalaxyProxy::GalaxyProxy(GalaxyProxyConfig config)
    : GalaxyProxy(std::move(config), *std::unique_ptr<Boot>(make_boot(config.model_string)))
{}

GalaxyProxy::GalaxyProxy(GalaxyProxyConfig config, Boot boot)
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
            control_value_element_size(bits.value_type),
            bits.read_only,
        };
    }
    dirty_controls_.assign(control_info_.size(), false);
    dirty_selectors_.assign(aem_.count_of(DESCRIPTOR_SIGNAL_SELECTOR), false);

    // Per-matrix cell wire type (SET_MATRIX self-writes encode with it).
    for (auto const& d : aem_.descriptors) {
        if (d.descriptor_type != DESCRIPTOR_MATRIX) {
            continue;
        }
        DescriptorMatrix m;
        span_load_padded(m, d.bytes);
        if (matrix_value_types_.size() <= d.descriptor_index) {
            matrix_value_types_.resize(d.descriptor_index + 1u, CONTROL_LINEAR_UINT8);
        }
        auto const bits = unpack_control_value_type(std::uint16_t(m.control_value_type.get()));
        matrix_value_types_[d.descriptor_index] = bits.value_type;
    }

    handler_.set_on_control_changed(
        [this](std::uint16_t idx, std::span<std::uint8_t const> values) { return on_northbound_control(idx, values); });
    handler_.set_on_matrix_changed([this](std::uint16_t idx, nanoavb::DescriptorStorageHandler::MatrixWrite const& w) {
        return on_northbound_matrix(idx, w);
    });
    handler_.set_on_signal_selector_changed(
        [this](std::uint16_t idx, SignalSource const& source) { return on_northbound_selector(idx, source); });
    liveness_ping_ = controlplane_devices::galaxy_describe("/status/model_string");
}

Eui64 GalaxyProxy::proxy_entity_id() const noexcept
{
    return eui64_from(derive_proxy_entity_id(config_.galaxy_entity_id));
}

bool GalaxyProxy::connect_southbound()
{
    using namespace controlplane_devices;
    if (!client_.connect(config_.galaxy_host, config_.galaxy_port)) {
        return false;
    }
    static char const* CATEGORIES[] = {"/processing/.*", "/device/.*", "/system/.*", "/project/.*", "/entity/.*", "/status/.*"};
    for (auto* pattern : CATEGORIES) {
        if (!client_.send(galaxy_get(pattern))) {
            return false;
        }
    }
    for (auto* pattern : CATEGORIES) {
        if (!client_.send(galaxy_subscribe(pattern, config_.subscribe_throttle_ms))) {
            return false;
        }
    }
    return true;
}

bool GalaxyProxy::send_southbound_set(ControlPointId h, double value)
{
    // Echo suppression: a value already at the mirrored state needs no set;
    // this is also what terminates the set -> echo -> re-apply loop, and
    // what makes write-current-value-back a safe northbound exercise.
    if (value == mirrored_value(mirror_, h)) {
        return true;
    }
    auto const& path = mirror_.ns().adaptor.address_of(h);
    auto formatted = format_value(mirror_.ns(), h, value);
    if (!client_.send(controlplane_devices::galaxy_set(path, formatted))) {
        return false;
    }
    // Optimistic local echo: the device does not push every change made
    // over this connection back to it, and a stale mirrored value here
    // suppressed the set that undoes a previous one (live incident: an
    // unmute never went southbound after its mute). A later device report
    // overwrites this if the device disagrees.
    mirror_.record_local_set(
        h,
        formatted,
        std::uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    ++southbound_sets_;
    return true;
}

std::uint8_t GalaxyProxy::on_northbound_control(std::uint16_t control_index, std::span<std::uint8_t const> values)
{
    if (applying_southbound_) {
        // Self-apply of mirrored device state: store + fan out as-is. The
        // northbound checks below guard controller writes only — vetoing
        // here would leave read-only controls (meters, telemetry) with no
        // stored state, and a mirrored active-snapshot id would loop back
        // into a spurious :recall_snapshot.
        return AEM_STATUS_SUCCESS;
    }
    if (control_index >= control_info_.size()) {
        return AEM_STATUS_NO_SUCH_DESCRIPTOR;
    }
    auto const& info = control_info_[control_index];
    auto const& ns = mirror_.ns();

    // Snapshot recall: the bound point is the read-only active-snapshot id;
    // a northbound write becomes the recall_snapshot built-in function.
    for (std::uint16_t v = 0; v < info.count; ++v) {
        auto bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_CONTROL, control_index, v});
        if (!bound) {
            continue;  // unbound value (e.g. IDENTIFY) — accept locally
        }
        auto wire = decode_element(info.value_type, values.subspan(v * info.element_size, info.element_size));
        if (ns.adaptor.address_of(*bound) == "/project/snapshot/active/id") {
            auto id = int(wire);
            if (id < 0) {
                return AEM_STATUS_BAD_ARGUMENTS;
            }
            if (!client_.send(":recall_snapshot " + std::to_string(id) + "\n")) {
                return AEM_STATUS_NOT_SUPPORTED;
            }
            ++southbound_sets_;
            continue;
        }
        if (ns.model->read_only(*bound)) {
            return AEM_STATUS_NOT_SUPPORTED;
        }
        if (!send_southbound_set(*bound, wire_to_schema_value(ns, *bound, wire))) {
            return AEM_STATUS_NOT_SUPPORTED;
        }
    }
    return AEM_STATUS_SUCCESS;
}

std::uint8_t GalaxyProxy::on_northbound_matrix(std::uint16_t matrix_index, nanoavb::DescriptorStorageHandler::MatrixWrite const& w)
{
    if (applying_southbound_) {
        return AEM_STATUS_SUCCESS;  // self-apply of mirrored device state
    }
    if (w.elem_size == 0 || w.value_count == 0) {
        return AEM_STATUS_BAD_ARGUMENTS;
    }
    // Walk the region in `direction` order (Table 7-146), skipping
    // item_offset cells; with rep the values repeat to fill the region.
    auto value_type = std::uint16_t(w.elem_size == 4 ? CONTROL_LINEAR_FLOAT : CONTROL_LINEAR_UINT8);
    std::size_t total = std::size_t(w.region.width) * w.region.height;
    std::size_t value_at = 0;
    for (std::size_t i = w.item_offset; i < total; ++i) {
        if (!w.rep && value_at >= w.value_count) {
            break;
        }
        std::uint16_t col, row;
        if (w.direction == 0) {
            col = std::uint16_t(w.region.column + (i % w.region.width));
            row = std::uint16_t(w.region.row + (i / w.region.width));
        } else {
            row = std::uint16_t(w.region.row + (i % w.region.height));
            col = std::uint16_t(w.region.column + (i / w.region.height));
        }
        auto wire =
            decode_element(value_type, w.values.subspan((w.rep ? value_at % w.value_count : value_at) * w.elem_size, w.elem_size));
        ++value_at;

        auto bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_MATRIX, matrix_index, std::uint16_t(row * MATRIX_WIDTH + col)});
        if (!bound) {
            return AEM_STATUS_BAD_ARGUMENTS;
        }
        if (!send_southbound_set(*bound, wire_to_schema_value(mirror_.ns(), *bound, wire))) {
            return AEM_STATUS_NOT_SUPPORTED;
        }
    }
    return AEM_STATUS_SUCCESS;
}

std::uint8_t GalaxyProxy::on_northbound_selector(std::uint16_t selector_index, SignalSource const& source)
{
    if (applying_southbound_) {
        return AEM_STATUS_SUCCESS;  // self-apply of mirrored device state
    }
    // The handler validated membership in the authored source list, so
    // this decodes unless the model shape changed under us.
    auto const mode = galaxy_input_mode_of(source);
    if (!mode.has_value()) {
        return AEM_STATUS_BAD_ARGUMENTS;
    }
    auto const bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_SIGNAL_SELECTOR, selector_index, 0});
    if (!bound) {
        return AEM_STATUS_NO_SUCH_DESCRIPTOR;
    }
    if (mirror_.ns().model->read_only(*bound)) {
        return AEM_STATUS_NOT_SUPPORTED;
    }
    return send_southbound_set(*bound, double(*mode)) ? AEM_STATUS_SUCCESS : AEM_STATUS_NOT_SUPPORTED;
}

void GalaxyProxy::apply_southbound_selector(std::uint16_t selector_index)
{
    auto const bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_SIGNAL_SELECTOR, selector_index, 0});
    if (!bound) {
        return;
    }
    auto const mode = mirrored_value(mirror_, *bound);
    if (mode < 0.0 || mode >= double(GALAXY_INPUT_MODE_COUNT)) {
        return;  // unmapped device state; leave the served selector alone
    }
    auto const source = galaxy_input_mode_source(std::uint16_t(mode));

    applying_southbound_ = true;
    if (running()) {
        // Store + unsolicited fan-out, exactly as a controller's SET would:
        // the AemSignalSelectorPayload shape (type, index, triple, reserved).
        std::vector<std::uint8_t> body;
        body.reserve(12);
        body.push_back(std::uint8_t(DESCRIPTOR_SIGNAL_SELECTOR >> 8));
        body.push_back(std::uint8_t(DESCRIPTOR_SIGNAL_SELECTOR & 0xFF));
        body.push_back(std::uint8_t(selector_index >> 8));
        body.push_back(std::uint8_t(selector_index & 0xFF));
        auto const triple = make_const_span(source);
        body.insert(body.end(), triple.begin(), triple.end());
        body.push_back(0);  // reserved
        body.push_back(0);
        (void)host_.components().aem_handler.apply_local_descriptor_value(AEM_COMMAND_SET_SIGNAL_SELECTOR, body);
    } else {
        // Offline (tests, pre-start): store only.
        nanoavb::DescriptorId id{{0, DESCRIPTOR_SIGNAL_SELECTOR, selector_index}, 0};
        (void)handler_.on_set_descriptor_value(AEM_COMMAND_SET_SIGNAL_SELECTOR, id, make_const_span(source));
    }
    applying_southbound_ = false;
}

void GalaxyProxy::apply_southbound_matrix_cell(std::uint16_t matrix_index, std::uint16_t cell)
{
    if (matrix_index >= matrix_value_types_.size()) {
        return;
    }
    auto const bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_MATRIX, matrix_index, cell});
    if (!bound) {
        return;
    }
    auto const value_type = matrix_value_types_[matrix_index];
    auto const schema = mirrored_value(mirror_, *bound);

    // A 1x1 SET_MATRIX region self-write at (column, row) = the cell's
    // coordinates: store + unsolicited fan-out, exactly as a controller's
    // SET would (Clause 7.4.33 region payload).
    std::vector<std::uint8_t> region;
    auto push16 = [&](std::uint16_t v) {
        region.push_back(std::uint8_t(v >> 8));
        region.push_back(std::uint8_t(v & 0xFF));
    };
    push16(std::uint16_t(cell % MATRIX_WIDTH));  // column
    push16(std::uint16_t(cell / MATRIX_WIDTH));  // row
    push16(1);                                   // width
    push16(1);                                   // height
    push16(1);                                   // rep off, horizontal, value_count 1
    push16(0);                                   // item_offset
    encode_element(value_type, schema_to_wire_value(mirror_.ns(), *bound, schema), region);

    applying_southbound_ = true;
    if (running()) {
        std::vector<std::uint8_t> body;
        body.reserve(4 + region.size());
        body.push_back(std::uint8_t(DESCRIPTOR_MATRIX >> 8));
        body.push_back(std::uint8_t(DESCRIPTOR_MATRIX & 0xFF));
        body.push_back(std::uint8_t(matrix_index >> 8));
        body.push_back(std::uint8_t(matrix_index & 0xFF));
        body.insert(body.end(), region.begin(), region.end());
        (void)host_.components().aem_handler.apply_local_descriptor_value(AEM_COMMAND_SET_MATRIX, body);
    } else {
        // Offline (tests, pre-start): store only.
        nanoavb::DescriptorId id{{0, DESCRIPTOR_MATRIX, matrix_index}, 0};
        (void)handler_.on_set_descriptor_value(AEM_COMMAND_SET_MATRIX, id, region);
    }
    applying_southbound_ = false;
}

void GalaxyProxy::apply_southbound_control(std::uint16_t control_index, std::uint64_t now_ns)
{
    (void)now_ns;
    auto const& info = control_info_[control_index];
    auto const& ns = mirror_.ns();

    std::vector<std::uint8_t> values;
    values.reserve(std::size_t(info.count) * info.element_size);
    for (std::uint16_t v = 0; v < info.count; ++v) {
        auto bound = aem_.binding.resolve(AemRef{0, DESCRIPTOR_CONTROL, control_index, v});
        double schema = bound ? mirrored_value(mirror_, *bound) : 0.0;
        encode_element(info.value_type, bound ? schema_to_wire_value(ns, *bound, schema) : 0.0, values);
    }

    applying_southbound_ = true;
    if (running()) {
        // Store + unsolicited fan-out, exactly as a controller's SET would.
        std::vector<std::uint8_t> body;
        body.reserve(4 + values.size());
        body.push_back(std::uint8_t(DESCRIPTOR_CONTROL >> 8));
        body.push_back(std::uint8_t(DESCRIPTOR_CONTROL & 0xFF));
        body.push_back(std::uint8_t(control_index >> 8));
        body.push_back(std::uint8_t(control_index & 0xFF));
        body.insert(body.end(), values.begin(), values.end());
        (void)host_.components().aem_handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, body);
    } else {
        // Offline (tests, pre-start): store only.
        nanoavb::DescriptorId id{{0, DESCRIPTOR_CONTROL, control_index}, 0};
        (void)handler_.on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, id, values);
    }
    applying_southbound_ = false;
}

int GalaxyProxy::poll(unsigned timeout_ms, std::uint64_t now_ns)
{
    int lines = client_.poll_lines(timeout_ms, [&](std::string_view line) {
        auto h = mirror_.consume_line(line, now_ns);
        if (!h) {
            return;
        }
        auto const& ref = aem_.binding.address_of(*h);
        if (ref.descriptor_type == std::uint16_t(DESCRIPTOR_CONTROL)) {
            dirty_controls_[ref.descriptor_index] = true;
        } else if (
            ref.descriptor_type == std::uint16_t(DESCRIPTOR_SIGNAL_SELECTOR) && ref.descriptor_index < dirty_selectors_.size()) {
            dirty_selectors_[ref.descriptor_index] = true;
        } else if (ref.descriptor_type == std::uint16_t(DESCRIPTOR_MATRIX)) {
            dirty_matrix_cells_.emplace_back(ref.descriptor_index, ref.value_index);
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
    if (lines == 0) {
        return 0;
    }
    for (std::uint16_t idx = 0; idx < dirty_controls_.size(); ++idx) {
        if (dirty_controls_[idx]) {
            dirty_controls_[idx] = false;
            apply_southbound_control(idx, now_ns);
        }
    }
    for (std::uint16_t idx = 0; idx < dirty_selectors_.size(); ++idx) {
        if (dirty_selectors_[idx]) {
            dirty_selectors_[idx] = false;
            apply_southbound_selector(idx);
        }
    }
    for (auto const& [matrix_index, cell] : dirty_matrix_cells_) {
        apply_southbound_matrix_cell(matrix_index, cell);
    }
    dirty_matrix_cells_.clear();
    return lines;
}

Status GalaxyProxy::start(net::MessageReactor& reactor, std::string_view interface_name)
{
    if (config_.mirror_avb_info) {
        host_.mirror_avb_info_from(
            eui64_from(config_.galaxy_entity_id), eui64_from(derive_proxy_entity_id(config_.galaxy_entity_id)));
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

Status GalaxyProxy::stop()
{
    return host_.stop_control_plane(sm::TimePoint{});
}

}  // namespace statusbar::galaxy_proxy
