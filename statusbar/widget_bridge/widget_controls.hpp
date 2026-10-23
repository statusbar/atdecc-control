// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

// CONTROL descriptor -> widget projection (docs/PLAN.md §12.2): pure
// functions from an AEM DescriptorControl to the widget spec the WS
// subprotocol serves, plus the value codecs for GET/SET_CONTROL bodies.
//
// v1 scope, deliberately honest: linear controls become sliders (or
// toggles when they span exactly {0,1}), numeric selectors become
// selects, CONTROL_UTF8 becomes read-only text, and everything else is
// listed as "opaque" — visible, never settable. All numeric traffic is
// carried as double (binary64 holds every linear type exactly except
// the int64/uint64 extremes, which no real control surface uses).

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/sg14/inplace_vector.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace statusbar::widget_bridge {

/// A single-value integer linear control whose exact state grid has at
/// most this many steps projects as a select over the grid instead of a
/// slider (a mode, a slope, a delay type); exactly two states (0/1, or
/// the AEM 0/255 convention) projects as a toggle.
constexpr int MAX_SELECT_STATES = 16;

enum class WidgetKind : std::uint8_t
{
    slider,
    toggle,
    select,
    text,
    meter,     ///< array-family control: a read-only bank of values with units
    matrix,    ///< MATRIX descriptor: a width x height crosspoint grid
    identify,  ///< the standard IDENTIFY control (Milan-mandated): a momentary wink button
    opaque,
};

[[nodiscard]] auto widget_kind_name(WidgetKind kind) noexcept -> std::string_view;

/// The widget-facing projection of one CONTROL descriptor.
struct ControlProjection
{
    WidgetKind kind = WidgetKind::opaque;
    std::uint16_t value_type = 0;  ///< masked CONTROL_* family value
    bool read_only = false;
    std::uint16_t count = 0;  ///< linear: entries; selector: 1

    // Linear family (first entry's shape; multi-entry linear controls are
    // served as `count` parallel values sharing it — the GALAXY authoring
    // convention).
    double minimum = 0.0;
    double maximum = 0.0;
    double step = 0.0;
    double default_value = 0.0;

    std::vector<double> current;  ///< linear: count values; selector: 1
    std::vector<double> options;  ///< selector option list

    /// Linear: each entry's localized_string reference (Table 7.13),
    /// parallel to `current` — a multi-value control names its values
    /// this way (the Meyer Temperature control: NVME / CPU / Case).
    /// Raw refs; the bridge resolves them against the entity's STRINGS.
    std::vector<std::uint16_t> value_string_refs;

    /// Display labels parallel to `options`, when a WidgetLabelProvider
    /// knows this control's enumeration; empty otherwise.
    std::vector<std::string> option_labels;

    std::string text;  ///< CONTROL_UTF8 current value

    std::uint8_t unit_code = 0;
    int unit_multiplier = 0;
};

/// Project @p d for the widget layer. Never fails: unsupported value
/// types come back as WidgetKind::opaque with only value_type/read_only
/// populated.
[[nodiscard]] auto project_control(atdecc::aem::DescriptorControl const& d) -> ControlProjection;

/// Encode @p values as a SET_CONTROL value body (the bytes after the
/// 4-byte control header) for @p d: linear expects exactly
/// number_of_values entries, each range-checked against min/max;
/// numeric selectors expect one value that matches an option. Returns
/// nullopt on a shape or range violation, or an unsupported type.
[[nodiscard]] auto encode_control_values(atdecc::aem::DescriptorControl const& d, std::span<double const> values)
    -> std::optional<statusbar::sg14::inplace_vector<std::uint8_t, 508>>;

/// Decode a GET/SET_CONTROL response value body for @p d into doubles
/// (linear: number_of_values elements; numeric selector: one). Returns
/// nullopt on a size mismatch or an unsupported type.
[[nodiscard]] auto decode_control_values(atdecc::aem::DescriptorControl const& d, std::span<std::uint8_t const> body)
    -> std::optional<std::vector<double>>;

/// Decode a run of linear-family elements of @p value_type (a MATRIX
/// region's values). Returns nullopt on a size misfit or non-linear type.
[[nodiscard]] auto decode_linear_elements(std::uint16_t value_type, std::span<std::uint8_t const> bytes)
    -> std::optional<std::vector<double>>;

/// Encode one linear-family element of @p value_type (a MATRIX cell
/// write). Returns nullopt for a non-linear type or a NaN.
[[nodiscard]] auto encode_linear_element(std::uint16_t value_type, double value)
    -> std::optional<statusbar::sg14::inplace_vector<std::uint8_t, 8>>;

}  // namespace statusbar::widget_bridge
