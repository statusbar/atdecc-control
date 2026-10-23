// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The Nadia CP (Meyer LCS Core) control-point namespace, generated from
// the Core control-point catalogue ("Nadia Control Points", 364 address
// patterns) — the mixer subset a 1722.1 controller can drive: the
// Input / Bus / Output / Listen / VGroup channel strips (level, trim,
// mute, solo, isolate, invert, delay, EQ, dynamics, meters, names), the
// Input×Bus and Bus×Output crosspoint matrices, and the System master.
// A live Core's loaded design (2026-09-23) extended this with the design-level
// processing families: VRAS room engines (their own input/return/output
// strips, reverb and early-reflection engines), Monitor strips, AGC
// units, and the Master select/mute section. Cues, SpaceMap,
// trajectories, WildTracks and the surface bookkeeping patterns remain
// deliberately absent.
//
// Unlike the GALAXY, whose namespace is fixed by the product, a Core's
// control points exist only for the channels the loaded mixer design
// declares — the catalogue gives the patterns, the design gives the
// counts. NadiaShape carries those counts; a proxy discovers them from
// the live device (see nadia_mirror_tool --discover) or takes them from
// the command line.

namespace statusbar::controlplane_devices {

/// How a point's text value is read and written. Every value crosses the
/// wire as text; the kind decides the scalar projection and the set
/// formatting.
enum class NadiaValueKind : std::uint8_t
{
    boolean,      ///< "true" / "false"
    decibels,     ///< float dB; the device may report "-inf" at the floor
    nanoseconds,  ///< int64 ns (delays, dynamics times)
    hertz,        ///< float Hz
    ratio,        ///< float (dynamics ratio, EQ Q)
    integer,      ///< int32 (EQ type and other enumerations)
    text,         ///< string (names, addresses)
};

/// Channel counts of the loaded mixer design. Zero disables a family.
struct NadiaShape
{
    std::uint16_t inputs = 0;
    std::uint16_t buses = 0;
    std::uint16_t outputs = 0;
    std::uint16_t listens = 0;
    std::uint16_t vgroups = 0;
    std::uint16_t eq_bands = 0;  ///< parametric bands per channel strip
    bool system = true;          ///< the System master points

    // Design-level processing families (a live Core's design vocabulary).
    std::uint16_t vras = 0;          ///< VRAS room engines
    std::uint16_t vras_inputs = 0;   ///< strips per engine
    std::uint16_t vras_returns = 0;  ///< strips per engine
    std::uint16_t vras_outputs = 0;  ///< per-engine output sends (128 per engine on the RZ — mind the size)
    std::uint16_t reflections = 0;   ///< early reflections per engine
    std::uint16_t monitors = 0;      ///< Monitor strips
    std::uint16_t agcs = 0;          ///< AGC units
    bool master = false;             ///< the Master select/mute section
};

/// One expanded control point.
struct NadiaPoint
{
    std::string path;  ///< canonical "/input/1/level"
    NadiaValueKind kind = NadiaValueKind::decibels;
    bool read_only = false;
    double min = 0.0;  ///< numeric kinds; min == max means unranged
    double max = 0.0;
};

/// A get/subscribe command covering one pattern's whole population
/// ("input 1-16 level"), so bulk reads stay one line per pattern.
struct NadiaPatternCommand
{
    std::string addresses;  ///< command form with ranges
    std::size_t first = 0;  ///< handle of the pattern's first point
    std::size_t count = 0;  ///< consecutive handles
};

std::vector<NadiaPoint> nadia_points(NadiaShape const& shape);

struct NadiaNamespace
{
    NadiaShape shape;
    std::shared_ptr<controlplane::Model const> model;
    controlplane::PathAdaptor adaptor;
    std::vector<NadiaValueKind> kinds;  ///< per handle
    std::vector<std::string> names;     ///< per handle: the friendly name the device reports
    std::vector<NadiaPatternCommand> patterns;

    /// Reported name -> handle (nadia_name_key match).
    std::optional<controlplane::ControlPointId> resolve_name(std::string_view friendly) const;

    /// Canonical path -> handle.
    std::optional<controlplane::ControlPointId> resolve_path(std::string_view path) const { return adaptor.resolve(path); }

    std::size_t size() const noexcept { return kinds.size(); }

    std::unordered_map<std::string, controlplane::ControlPointId> name_index;  ///< nadia_name_key -> handle
};

NadiaNamespace build_nadia_namespace(NadiaShape const& shape);

/// The scalar the Instance stores for a reported text value: booleans
/// 0/1, numbers as themselves (a leading number with a trailing unit
/// suffix parses; "-inf" is the point's floor), nullopt for text and
/// unparseable values.
std::optional<double> nadia_scalar_of(NadiaValueKind kind, double floor, std::string_view value);

/// The text a set command carries for a scalar of the given kind.
std::string nadia_format_value(NadiaValueKind kind, double value);

}  // namespace statusbar::controlplane_devices
