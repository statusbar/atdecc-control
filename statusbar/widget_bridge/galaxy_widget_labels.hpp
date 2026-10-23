// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

// GALAXY label provider (docs/PLAN.md §12.2): recognizes the galaxy
// proxy's entity_model_ids and answers option labels from the folded
// schema enum tables (the device's own "value:NAME" describe labels).
//
// The proxy's AEM model is generated deterministically from the GALAXY
// namespace, so this provider regenerates the same model per
// entity_model_id variant and walks CONTROL descriptor index ->
// AemBinding -> ControlPointId -> GalaxyNamespace::enum_labels. The
// build is heavy (9766 points) and lazy: nothing happens until a
// matching entity_model_id is actually seen.

#include "statusbar/widget_bridge/widget_labels.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace statusbar::widget_bridge {

class GalaxyWidgetLabels final : public WidgetLabelProvider
{
  public:
    GalaxyWidgetLabels();
    ~GalaxyWidgetLabels() override;

    [[nodiscard]] auto options_for(std::uint64_t entity_model_id, std::uint16_t control_index, bool selector)
        -> std::vector<WidgetOption> override;

    [[nodiscard]] auto matrix_row_labels(std::uint64_t entity_model_id, std::uint16_t matrix_index, std::uint16_t row_count)
        -> std::vector<std::string> override;

  private:
    struct Built;
    auto built_for(std::uint64_t entity_model_id) -> Built const*;

    std::map<std::uint64_t, std::unique_ptr<Built>> built_;  ///< nullptr = known non-galaxy id
};

}  // namespace statusbar::widget_bridge
