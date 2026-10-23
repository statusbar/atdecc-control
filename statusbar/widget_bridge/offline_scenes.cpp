// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/widget_bridge/offline_scenes.hpp"

#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/controlplane/controlplane_scene.hpp"
#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"
#include "statusbar/http/http_server.hpp"
#include "statusbar/widget_bridge/widget_controls.hpp"

#include <array>
#include <chrono>
#include <format>
#include <map>
#include <vector>

namespace statusbar::widget_bridge {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using controlplane::AemRef;
using controlplane::ControlPointId;

namespace {

constexpr std::size_t MATRIX_WIDTH = 16;

/// Boolean points travel as true/false on the schema side, 0/255 in AEM.
auto is_boolean_point(controlplane_devices::GalaxyNamespace const& ns, ControlPointId h) -> bool
{
    auto const& def = ns.defaults[h];
    return def == "true" || def == "false";
}

/// The (value_type, count) of a CONTROL descriptor, from the blob.
struct ControlShape
{
    std::uint16_t value_type{0};
    std::uint16_t count{0};
};

auto control_shape(nanoavb::AemEntityModel const& model, std::uint16_t index) -> std::optional<ControlShape>
{
    auto const* storage = model.static_store();
    if (storage == nullptr) {
        return std::nullopt;
    }
    auto const blob = storage->get_descriptor(0, DESCRIPTOR_CONTROL, index);
    if (!blob.has_value() || blob->size() < DescriptorControl::LENGTH) {
        return std::nullopt;
    }
    DescriptorControl d{};
    span_load_padded(d, *blob);
    auto const bits = unpack_control_value_type(d.control_value_type.get());
    return ControlShape{bits.value_type, d.number_of_values.get()};
}

}  // namespace

// ---- HTTP handlers ----------------------------------------------------------

class OfflineGalaxyScenes::CaptureHandler final : public http::HttpHandler
{
  public:
    explicit CaptureHandler(OfflineGalaxyScenes& owner)
        : owner_{owner}
    {}
    auto on_headers(http::HttpRequest const&, http::ResponseWriter&) -> http::Disposition override
    {
        return http::Disposition::buffer();
    }
    void on_complete(http::HttpRequest const&, http::ResponseWriter& writer) override
    {
        // A full scene (~100 KiB) exceeds the inline tx buffer, so the
        // body goes out by reference; the per-slot slot in bodies_ keeps
        // it alive until the response finishes (replaced only by the same
        // slot's next request, after that response completed).
        auto& text = bodies_[writer.slot()];
        text = owner_.capture_text();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        (void)writer.send_external(
            200, "text/plain; charset=utf-8", {reinterpret_cast<std::uint8_t const*>(text.data()), text.size()});
    }

  private:
    OfflineGalaxyScenes& owner_;
    std::map<std::size_t, std::string> bodies_;
};

class OfflineGalaxyScenes::RecallHandler final : public http::HttpHandler
{
  public:
    explicit RecallHandler(OfflineGalaxyScenes& owner)
        : owner_{owner}
    {}
    auto on_headers(http::HttpRequest const&, http::ResponseWriter&) -> http::Disposition override
    {
        return http::Disposition::buffer();
    }
    void on_body_chunk(http::HttpRequest const&, std::span<std::uint8_t const> chunk) override
    {
        body_.assign(chunk.begin(), chunk.end());
    }
    void on_complete(http::HttpRequest const&, http::ResponseWriter& writer) override
    {
        auto const report = owner_.recall_text(std::string_view{body_});
        body_.clear();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        (void)writer.send(200, "text/plain; charset=utf-8", {reinterpret_cast<std::uint8_t const*>(report.data()), report.size()});
    }

  private:
    OfflineGalaxyScenes& owner_;
    std::string body_;
};

// ---- component --------------------------------------------------------------

OfflineGalaxyScenes::OfflineGalaxyScenes(
    controlplane_devices::GalaxyNamespace const& ns,
    controlplane::AemBinding const& binding,
    OfflineControllerService& service,
    WidgetBridge& bridge)
    : ns_{ns}
    , binding_{binding}
    , service_{service}
    , bridge_{bridge}
    , capture_{std::make_unique<CaptureHandler>(*this)}
    , recall_{std::make_unique<RecallHandler>(*this)}
{}

auto OfflineGalaxyScenes::register_routes() -> bool
{
    auto const ok = bridge_.http_server().add_route(http::HttpMethod::get, "/scene/capture", *capture_) &&
        bridge_.http_server().add_route(http::HttpMethod::post, "/scene/recall", *recall_);
    if (ok) {
        bridge_.set_scene_endpoints();
    }
    return ok;
}

auto OfflineGalaxyScenes::read_point(ControlPointId h) -> std::optional<double>
{
    auto const& ref = binding_.address_of(h);
    auto& handler = service_.handler();
    auto& model = service_.model();

    if (ref.descriptor_type == DESCRIPTOR_CONTROL) {
        auto const shape = control_shape(model, ref.descriptor_index);
        if (!shape.has_value() || ref.value_index >= shape->count) {
            return std::nullopt;
        }
        std::array<std::uint8_t, 4> header{
            0x00, 0x1A, std::uint8_t(ref.descriptor_index >> 8), std::uint8_t(ref.descriptor_index & 0xFF)};
        std::array<std::uint8_t, 2048> out{};
        auto const n = model.get_descriptor_value_for_wire(AEM_COMMAND_GET_CONTROL, header, out);
        if (n < 4) {
            return std::nullopt;
        }
        auto const values = decode_linear_elements(shape->value_type, std::span<std::uint8_t const>{out}.subspan(4, n - 4));
        if (!values.has_value() || ref.value_index >= values->size()) {
            return std::nullopt;
        }
        auto const wire = (*values)[ref.value_index];
        return is_boolean_point(ns_, h) ? (wire != 0.0 ? 1.0 : 0.0) : wire;
    }

    if (ref.descriptor_type == DESCRIPTOR_MATRIX) {
        // A 1x1 region read of the bound cell.
        std::vector<std::uint8_t> request;
        auto push16 = [&](std::uint16_t v) {
            request.push_back(std::uint8_t(v >> 8));
            request.push_back(std::uint8_t(v & 0xFF));
        };
        push16(DESCRIPTOR_MATRIX);
        push16(ref.descriptor_index);
        push16(std::uint16_t(ref.value_index % MATRIX_WIDTH));
        push16(std::uint16_t(ref.value_index / MATRIX_WIDTH));
        push16(1);
        push16(1);
        push16(1);
        push16(0);
        std::array<std::uint8_t, 64> out{};
        auto const n = model.get_descriptor_value_for_wire(AEM_COMMAND_GET_MATRIX, request, out);
        if (n < 4 + 12 + 1) {
            return std::nullopt;
        }
        // Cell type from the MATRIX descriptor.
        auto const* storage = model.static_store();
        if (storage == nullptr) {
            return std::nullopt;
        }
        auto const blob = storage->get_descriptor(0, DESCRIPTOR_MATRIX, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < DescriptorMatrix::LENGTH) {
            return std::nullopt;
        }
        DescriptorMatrix m{};
        span_load_padded(m, *blob);
        auto const bits = unpack_control_value_type(m.control_value_type.get());
        auto const values = decode_linear_elements(bits.value_type, std::span<std::uint8_t const>{out}.subspan(16, n - 16));
        if (!values.has_value() || values->empty()) {
            return std::nullopt;
        }
        auto const wire = values->front();
        return is_boolean_point(ns_, h) ? (wire != 0.0 ? 1.0 : 0.0) : wire;
    }

    if (ref.descriptor_type == DESCRIPTOR_SIGNAL_SELECTOR) {
        auto const current = handler.current_selector_source(
            {.configuration_index = 0, .descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR, .descriptor_index = ref.descriptor_index});
        if (!current.has_value()) {
            return std::nullopt;
        }
        auto const mode = galaxy_proxy::galaxy_input_mode_of(*current);
        return mode.has_value() ? std::optional{double(*mode)} : std::nullopt;
    }

    return std::nullopt;
}

auto OfflineGalaxyScenes::write_point(ControlPointId h, double value) -> bool
{
    auto const& ref = binding_.address_of(h);
    auto& model = service_.model();
    double const wire = is_boolean_point(ns_, h) ? (value != 0.0 ? 255.0 : 0.0) : value;

    std::array<std::uint8_t, 2048> echo{};
    auto push_header = [&](std::vector<std::uint8_t>& body, std::uint16_t type, std::uint16_t index) {
        body.push_back(std::uint8_t(type >> 8));
        body.push_back(std::uint8_t(type & 0xFF));
        body.push_back(std::uint8_t(index >> 8));
        body.push_back(std::uint8_t(index & 0xFF));
    };

    if (ref.descriptor_type == DESCRIPTOR_CONTROL) {
        auto const shape = control_shape(model, ref.descriptor_index);
        if (!shape.has_value() || ref.value_index >= shape->count) {
            return false;
        }
        // Read-modify-write: SET_CONTROL carries every value of the control.
        std::array<std::uint8_t, 4> header{
            0x00, 0x1A, std::uint8_t(ref.descriptor_index >> 8), std::uint8_t(ref.descriptor_index & 0xFF)};
        std::array<std::uint8_t, 2048> current{};
        auto const n = model.get_descriptor_value_for_wire(AEM_COMMAND_GET_CONTROL, header, current);
        if (n < 4) {
            return false;
        }
        auto values = decode_linear_elements(shape->value_type, std::span<std::uint8_t const>{current}.subspan(4, n - 4));
        if (!values.has_value() || ref.value_index >= values->size()) {
            return false;
        }
        (*values)[ref.value_index] = wire;
        std::vector<std::uint8_t> body;
        push_header(body, DESCRIPTOR_CONTROL, ref.descriptor_index);
        for (auto const v : *values) {
            auto const encoded = encode_linear_element(shape->value_type, v);
            if (!encoded.has_value()) {
                return false;
            }
            body.insert(body.end(), encoded->begin(), encoded->end());
        }
        return model.apply_set_descriptor_value(AEM_COMMAND_SET_CONTROL, body, echo).status == AEM_STATUS_SUCCESS;
    }

    if (ref.descriptor_type == DESCRIPTOR_MATRIX) {
        auto const* storage = model.static_store();
        if (storage == nullptr) {
            return false;
        }
        auto const blob = storage->get_descriptor(0, DESCRIPTOR_MATRIX, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < DescriptorMatrix::LENGTH) {
            return false;
        }
        DescriptorMatrix m{};
        span_load_padded(m, *blob);
        auto const bits = unpack_control_value_type(m.control_value_type.get());
        auto const encoded = encode_linear_element(bits.value_type, wire);
        if (!encoded.has_value()) {
            return false;
        }
        std::vector<std::uint8_t> body;
        push_header(body, DESCRIPTOR_MATRIX, ref.descriptor_index);
        auto push16 = [&](std::uint16_t v) {
            body.push_back(std::uint8_t(v >> 8));
            body.push_back(std::uint8_t(v & 0xFF));
        };
        push16(std::uint16_t(ref.value_index % MATRIX_WIDTH));
        push16(std::uint16_t(ref.value_index / MATRIX_WIDTH));
        push16(1);
        push16(1);
        push16(1);
        push16(0);
        body.insert(body.end(), encoded->begin(), encoded->end());
        return model.apply_set_descriptor_value(AEM_COMMAND_SET_MATRIX, body, echo).status == AEM_STATUS_SUCCESS;
    }

    if (ref.descriptor_type == DESCRIPTOR_SIGNAL_SELECTOR) {
        if (value < 0.0 || value >= double(galaxy_proxy::GALAXY_INPUT_MODE_COUNT)) {
            return false;
        }
        auto const source = galaxy_proxy::galaxy_input_mode_source(std::uint16_t(value));
        std::vector<std::uint8_t> body;
        push_header(body, DESCRIPTOR_SIGNAL_SELECTOR, ref.descriptor_index);
        auto const triple = make_const_span(source);
        body.insert(body.end(), triple.begin(), triple.end());
        body.push_back(0);
        body.push_back(0);
        return model.apply_set_descriptor_value(AEM_COMMAND_SET_SIGNAL_SELECTOR, body, echo).status == AEM_STATUS_SUCCESS;
    }

    return false;
}

auto OfflineGalaxyScenes::capture_text() -> std::string
{
    auto const points = ns_.size();
    controlplane::Scene scene;
    scene.name = std::format(
        "offline-capture-{}",
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    scene.mask.resize(points);
    scene.values.assign(points, 0);
    for (ControlPointId h = 0; h < points; ++h) {
        if (ns_.model->read_only(h) || binding_.address_of(h).descriptor_type == AemRef::NO_DESCRIPTOR) {
            continue;
        }
        auto const value = read_point(h);
        if (!value.has_value()) {
            continue;
        }
        scene.mask.set(h);
        scene.values[h] = controlplane::value_bits(*value);
    }
    return controlplane::scene_to_text(scene, ns_.adaptor, "galaxy-816");
}

auto OfflineGalaxyScenes::recall_text(std::string_view text) -> std::string
{
    auto resolution = controlplane::scene_from_text(text, ns_.adaptor);
    std::size_t applied = 0;
    std::size_t failed = 0;
    std::size_t read_only = 0;
    for (ControlPointId h = 0; h < resolution.scene.mask.size(); ++h) {
        if (!resolution.scene.mask.test(h)) {
            continue;
        }
        if (ns_.model->read_only(h)) {
            ++read_only;
            continue;
        }
        if (write_point(h, controlplane::value_double(resolution.scene.values[h]))) {
            ++applied;
        } else {
            ++failed;
        }
    }
    bridge_.refresh_entity_values(0);
    return std::format(
        "recall: matched {} applied {} read-only {} failed {} missing {}\n",
        resolution.matched,
        applied,
        read_only,
        failed,
        resolution.missing.size());
}

}  // namespace statusbar::widget_bridge
