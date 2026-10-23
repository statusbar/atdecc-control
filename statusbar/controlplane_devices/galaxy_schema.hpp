// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The GALAXY 816 control-point namespace, generated from the Galileo GALAXY
// Programming Guide (PN 05.230.008.01 Rev B4 and the 2026-08-23 web
// edition; the two agree on every normalized path shape — see
// python/galaxy/check_namespace.py for the cross-check and the curated
// list of PDF-extraction artifact corrections).
//
// Defaults are folded from the guide-corroborated live dump
// (captures/q1-live-dump.tsv): factory/representative firmware 2.10
// state, except where fold_describes.py's LIVE_DRIFT table marks the
// dump value as operator/venue state on the in-use unit — there the
// guide default in the template table stands. /entity and /status
// defaults are this Q1's real values, unique per physical device in
// reality. Built-in
// functions (:recall_snapshot and friends) are commands, not control
// points, and are deliberately absent; /processing/matrix/Matrix, an
// artifact row present in both document editions, is excluded.

namespace statusbar::controlplane_devices {

/// One expanded control point: the concrete path, its guide default
/// (unquoted; the live-dump fold may override it in the built
/// namespace), and whether the guide marks it read-only (/status).
struct GalaxyPoint
{
    std::string path;
    std::string default_value;
    bool read_only = false;
};

/// Expands the guide's template namespace into every concrete control
/// point, in deterministic canonical order (the handle-assignment walk).
std::vector<GalaxyPoint> galaxy_816_points();

/// The namespace bound to the controlplane core: one Model (read_only
/// flags, numeric defaults where parseable), the path adaptor, and the
/// per-handle string defaults and device names used by the describe
/// response.
struct GalaxyNamespace
{
    std::shared_ptr<controlplane::Model const> model;
    controlplane::PathAdaptor adaptor;
    std::vector<std::string> defaults;  ///< per handle, unquoted

    /// Per handle: the device's verbatim human-readable name (its describe
    /// `name`/`description` fields, which are identical — including the
    /// leading space the firmware leaves when its name prefix is empty).
    std::vector<std::string> descriptions;

    /// Per handle: the device's enum label table ("value:NAME" strings,
    /// static storage) — empty span for non-enumerated points.
    std::vector<std::pair<char const* const*, std::uint16_t>> enums;

    std::span<char const* const> enum_labels(controlplane::ControlPointId h) const { return {enums[h].first, enums[h].second}; }

    /// The device's `@N` alias numbers (its own item indices, folded from
    /// the live describe captures): handle -> alias and alias -> handle.
    std::vector<std::uint16_t> aliases;
    std::vector<controlplane::ControlPointId> alias_handles;

    std::uint16_t alias_of(controlplane::ControlPointId h) const { return aliases[h]; }
    controlplane::ControlPointId handle_of_alias(std::uint16_t alias) const { return alias_handles[alias]; }

    /// Resolves an ASCII address: a canonical /path, or the device's @N
    /// alias form. Patterns are not addresses — see resolve_selection.
    std::optional<controlplane::ControlPointId> resolve_address(std::string_view address) const;

    /// Resolves an ASCII selection argument: @N selects that one point;
    /// anything else goes through the pattern grammar (`*` any segment,
    /// `#` numeric — an exact path is the degenerate pattern).
    controlplane::Selector resolve_selection(std::string_view address_or_pattern) const;

    std::size_t size() const noexcept { return defaults.size(); }
};

GalaxyNamespace build_galaxy_816_namespace();

/// The `?` describe response for every handle in the selector, in the
/// guide's exact shape:
///
///   ?{'/path': { 'description' : 'Input 1 Mute', 'read_only' : 'false',
///     'name' : 'item_0', 'value' : 'false', 'minimum' : '', 'maximum' :
///     '', 'default' : 'false', 'step' : '', 'units' : ''}, ...}
///
/// Every value is a quoted string — that is what keeps the response
/// parseable as both a Python literal and a JavaScript expression; do not
/// "improve" it into bare literals. minimum/maximum/step/units are empty
/// until the live-device milestone fills them in.
std::string galaxy_describe_response(GalaxyNamespace const& ns, controlplane::Selector const& points);

std::string galaxy_describe_response(GalaxyNamespace const& ns, controlplane::ControlPointId h);

}  // namespace statusbar::controlplane_devices
