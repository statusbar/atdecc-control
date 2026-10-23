// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

// Widget label provider (docs/PLAN.md §12.2): the seam through which
// per-option display labels reach the widget layer. AEM has no
// per-option label mechanism — linear controls carry only ranges, and
// SELECTOR options are bare values — so labels must come from outside
// the wire model: a schema the bridge recognizes by entity_model_id
// (the GALAXY proxy's generated model), a config file, or nothing.
//
// A provider that returns options for a control does more than
// decorate: the bridge upgrades that control to a `select` regardless
// of its state-grid size, because an authoritative enumeration beats
// the range heuristic (the GALAXY's delay-integration type is a
// 162-entry enum that a range check would call a slider).

#include <cstdint>
#include <string>
#include <vector>

namespace statusbar::widget_bridge {

struct WidgetOption
{
    double value = 0.0;
    std::string label;
};

class WidgetLabelProvider
{
  public:
    virtual ~WidgetLabelProvider() = default;

    /// The enumerated options of one CONTROL descriptor (or, with
    /// @p selector, one SIGNAL_SELECTOR — where option values are
    /// positions in the authored source list), or empty when this
    /// provider knows nothing about it. Called at entity build time.
    [[nodiscard]] virtual auto options_for(std::uint64_t entity_model_id, std::uint16_t control_index, bool selector)
        -> std::vector<WidgetOption> = 0;

    /// Display labels for a MATRIX's rows (its sources), parallel to
    /// 0..row_count-1; empty = no labels known. AEM cannot carry these:
    /// MATRIX_SIGNAL descriptors are nameless, so row identity is schema
    /// knowledge.
    [[nodiscard]] virtual auto matrix_row_labels(std::uint64_t entity_model_id, std::uint16_t matrix_index, std::uint16_t row_count)
        -> std::vector<std::string>
    {
        (void)entity_model_id, (void)matrix_index, (void)row_count;
        return {};
    }
};

}  // namespace statusbar::widget_bridge
