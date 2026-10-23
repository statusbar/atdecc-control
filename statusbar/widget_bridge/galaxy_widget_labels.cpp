// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/widget_bridge/galaxy_widget_labels.hpp"

#include "statusbar/controlplane/controlplane_aem_binding.hpp"
#include "statusbar/controlplane_devices/galaxy_schema.hpp"
#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"

#include <cstdlib>
#include <string_view>

namespace statusbar::widget_bridge {

namespace {

/// AEM descriptor types (numeric to keep this file free of avb headers
/// beyond what galaxy_proxy already pulls in).
constexpr std::uint16_t DESCRIPTOR_CONTROL_TYPE = 0x001a;
constexpr std::uint16_t DESCRIPTOR_SIGNAL_SELECTOR_TYPE = 0x001b;

auto model_string_for(std::uint64_t entity_model_id) -> char const*
{
    if (entity_model_id == galaxy_proxy::GALAXY_PROXY_ENTITY_MODEL_ID_GX816) {
        return "GX-816";
    }
    if (entity_model_id == galaxy_proxy::GALAXY_PROXY_ENTITY_MODEL_ID_GX408) {
        return "GX-408";
    }
    if (entity_model_id == galaxy_proxy::GALAXY_PROXY_ENTITY_MODEL_ID_GX816AES3) {
        return "GX-816-AES3";
    }
    return nullptr;
}

}  // namespace

/// One regenerated model variant: the namespace (labels) plus the
/// binding (descriptor index -> handle).
struct GalaxyWidgetLabels::Built
{
    controlplane_devices::GalaxyNamespace ns;
    controlplane::AemBinding binding;
};

GalaxyWidgetLabels::GalaxyWidgetLabels() = default;
GalaxyWidgetLabels::~GalaxyWidgetLabels() = default;

auto GalaxyWidgetLabels::built_for(std::uint64_t entity_model_id) -> Built const*
{
    auto const it = built_.find(entity_model_id);
    if (it != built_.end()) {
        return it->second.get();
    }
    auto const* model_string = model_string_for(entity_model_id);
    if (model_string == nullptr) {
        built_.emplace(entity_model_id, nullptr);  // remember the miss
        return nullptr;
    }
    auto built = std::make_unique<Built>();
    built->ns = controlplane_devices::build_galaxy_816_namespace();
    auto model = galaxy_proxy::build_galaxy_proxy_aem_model(built->ns, galaxy_proxy::galaxy_model_banks(model_string));
    built->binding = std::move(model.binding);
    return built_.emplace(entity_model_id, std::move(built)).first->second.get();
}

auto GalaxyWidgetLabels::options_for(std::uint64_t entity_model_id, std::uint16_t control_index, bool selector)
    -> std::vector<WidgetOption>
{
    auto const* built = built_for(entity_model_id);
    if (built == nullptr) {
        return {};
    }
    // For SIGNAL_SELECTORs the galaxy model authors sources with
    // signal_index == mode value == source-list position, so the enum
    // labels' values double as positions — exactly the contract the
    // selector side of WidgetLabelProvider wants.
    auto const descriptor_type = selector ? DESCRIPTOR_SIGNAL_SELECTOR_TYPE : DESCRIPTOR_CONTROL_TYPE;
    auto const handle = built->binding.resolve(controlplane::AemRef{
        .configuration = 0, .descriptor_type = descriptor_type, .descriptor_index = control_index, .value_index = 0});
    if (!handle.has_value()) {
        return {};
    }
    auto const labels = built->ns.enum_labels(*handle);
    std::vector<WidgetOption> out;
    out.reserve(labels.size());
    for (char const* const entry : labels) {
        // "value:NAME" — the device's verbatim describe labels.
        std::string_view const text{entry};
        auto const colon = text.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        char* end = nullptr;
        double const value = std::strtod(entry, &end);
        if (end != entry + colon) {
            continue;
        }
        out.push_back({.value = value, .label = std::string{text.substr(colon + 1)}});
    }
    return out;
}

auto GalaxyWidgetLabels::matrix_row_labels(std::uint64_t entity_model_id, std::uint16_t matrix_index, std::uint16_t row_count)
    -> std::vector<std::string>
{
    // The GALAXY matrix's 32 sources are the 8 processed device inputs
    // followed by the 24 matrix inputs (PLAN §3/§5.2 — the guide's "8
    // device inputs + 24 matrix inputs"). The device's own describe
    // names carry no per-row identity (" Gain"), so this is schema
    // knowledge, shared by all four crosspoint-parameter matrices.
    (void)matrix_index;
    if (model_string_for(entity_model_id) == nullptr || row_count != 32) {
        return {};
    }
    std::vector<std::string> labels;
    labels.reserve(row_count);
    for (unsigned i = 1; i <= 8; ++i) {
        labels.push_back("Input " + std::to_string(i));
    }
    for (unsigned i = 1; i <= 24; ++i) {
        labels.push_back("Matrix In " + std::to_string(i));
    }
    return labels;
}

}  // namespace statusbar::widget_bridge
