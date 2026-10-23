// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the Nadia CP namespace: catalogue expansion against a
// design shape, pattern commands, name resolution, value projection.

#include "statusbar/controlplane_devices/nadia_schema.hpp"

#include "statusbar/test/test.hpp"

#include <algorithm>

using namespace statusbar;
using namespace statusbar::controlplane_devices;

namespace {

NadiaShape small_shape()
{
    NadiaShape s;
    s.inputs = 2;
    s.buses = 2;
    s.outputs = 1;
    s.listens = 0;
    s.vgroups = 1;
    s.eq_bands = 2;
    s.system = true;
    return s;
}

std::size_t count_prefix(std::vector<NadiaPoint> const& points, std::string_view prefix)
{
    return std::size_t(std::count_if(
        points.begin(), points.end(), [&](NadiaPoint const& p) { return p.path.compare(0, prefix.size(), prefix) == 0; }));
}

NadiaPatternCommand const* find_pattern(NadiaNamespace const& ns, std::string_view addresses)
{
    for (auto const& p : ns.patterns) {
        if (p.addresses == addresses) {
            return &p;
        }
    }
    return nullptr;
}

}  // namespace

TEST(nadia_schema, strip_expansion_counts)
{
    // One input strip with no EQ bands and no system: 51 strip points plus
    // the pre/post fader meters.
    NadiaShape s;
    s.inputs = 1;
    s.system = false;
    auto pts = nadia_points(s);
    EXPECT_EQ(pts.size(), std::size_t(53));

    // Each parametric band adds six points.
    s.eq_bands = 3;
    EXPECT_EQ(nadia_points(s).size(), std::size_t(53 + 18));

    // VGroups carry only the mix-level set.
    NadiaShape v;
    v.vgroups = 4;
    v.system = false;
    EXPECT_EQ(nadia_points(v).size(), std::size_t(4 * 12));

    // An empty design leaves only System.
    NadiaShape none;
    EXPECT_EQ(nadia_points(none).size(), std::size_t(9));
    none.system = false;
    EXPECT_TRUE(nadia_points(none).empty());
}

TEST(nadia_schema, design_level_families)
{
    NadiaShape shape;
    shape.outputs = 2;
    shape.eq_bands = 2;
    shape.vras = 2;
    shape.vras_inputs = 3;
    shape.vras_returns = 2;
    shape.reflections = 4;
    shape.monitors = 2;
    shape.agcs = 1;
    shape.master = true;
    shape.system = false;
    auto ns = build_nadia_namespace(shape);
    // Representative paths from every new family resolve.
    EXPECT_TRUE(ns.resolve_path("/vras/2/input/3/level").has_value());
    EXPECT_TRUE(ns.resolve_path("/vras/1/return/2/ushaping/4/frequency").has_value());
    EXPECT_TRUE(ns.resolve_path("/vras/1/return/2/ushaping/5/level").has_value());
    EXPECT_TRUE(ns.resolve_path("/vras/2/reverb/duration").has_value());
    EXPECT_TRUE(ns.resolve_path("/vras/1/reverb/2/damping/frequency").has_value());
    EXPECT_TRUE(ns.resolve_path("/vras/1/reverb/5/damping/parametric/beta").has_value());
    EXPECT_TRUE(ns.resolve_path("/vras/2/earlyreflection/4/delay").has_value());
    EXPECT_TRUE(ns.resolve_path("/monitor/2/dynamics/compressor/threshold").has_value());
    EXPECT_TRUE(ns.resolve_path("/agc/1/parametric/2/frequency").has_value());
    EXPECT_TRUE(ns.resolve_path("/master/output/mute").has_value());
    EXPECT_TRUE(ns.resolve_path("/master/vras/2/return/mute").has_value());
    // Outputs now carry U-Shaping; VRAS outputs stay opt-in (0 here).
    EXPECT_TRUE(ns.resolve_path("/output/1/ushaping/1/slope").has_value());
    EXPECT_FALSE(ns.resolve_path("/vras/1/output/1/level").has_value());
    // The ranged command grammar matches the device ("vras 1-2 reverb duration").
    bool found = false;
    for (auto const& cmd : ns.patterns) {
        if (cmd.addresses == "vras 1-2 reverb duration") {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(nadia_schema, crosspoints_and_system)
{
    auto pts = nadia_points(small_shape());
    EXPECT_EQ(count_prefix(pts, "/input/1/bus/"), std::size_t(1 + 3 * 2));
    EXPECT_EQ(count_prefix(pts, "/bus/2/output/"), std::size_t(1 + 8 * 1));
    EXPECT_EQ(count_prefix(pts, "/system/"), std::size_t(9));
    EXPECT_EQ(count_prefix(pts, "/listen/"), std::size_t(0));

    // Unique canonical paths.
    std::vector<std::string> paths;
    for (auto const& p : pts) {
        paths.push_back(p.path);
    }
    std::sort(paths.begin(), paths.end());
    EXPECT_TRUE(std::adjacent_find(paths.begin(), paths.end()) == paths.end());
}

TEST(nadia_schema, namespace_lookup)
{
    auto ns = build_nadia_namespace(small_shape());
    EXPECT_EQ(ns.size(), nadia_points(small_shape()).size());
    EXPECT_EQ(ns.kinds.size(), ns.size());
    EXPECT_EQ(ns.names.size(), ns.size());

    auto h = ns.resolve_path("/bus/2/output/1/level");
    EXPECT_TRUE(h.has_value());
    EXPECT_TRUE(ns.names[*h] == "Bus 2 Output 1 Level");
    EXPECT_TRUE(ns.kinds[*h] == NadiaValueKind::decibels);
    EXPECT_TRUE(ns.resolve_name("Bus 2 Output 1 Level") == h);
    EXPECT_TRUE(ns.resolve_name("bus 2 output 1 level") == h);
    EXPECT_FALSE(ns.resolve_name("Bus 3 Output 1 Level").has_value());
    EXPECT_FALSE(ns.resolve_path("/bus/2/output/2/level").has_value());

    auto ro = ns.resolve_path("/input/1/effective/level");
    EXPECT_TRUE(ro.has_value());
    EXPECT_TRUE(ns.model->read_only(*ro));
    EXPECT_FALSE(ns.model->read_only(*h));
    EXPECT_EQ(ns.model->min(*h), -90.0);
    EXPECT_EQ(ns.model->max(*h), 10.0);

    auto name = ns.resolve_path("/input/2/name");
    EXPECT_TRUE(name.has_value());
    EXPECT_TRUE(ns.kinds[*name] == NadiaValueKind::text);
    EXPECT_TRUE(ns.resolve_name("Input 2 Name") == name);

    auto gate = ns.resolve_path("/output/1/dynamics/noisegate/threshold");
    EXPECT_TRUE(gate.has_value());
    EXPECT_TRUE(ns.names[*gate] == "Output 1 Dynamics NoiseGate Threshold");
}

TEST(nadia_schema, pattern_commands)
{
    auto ns = build_nadia_namespace(small_shape());
    // Every point belongs to exactly one pattern, in handle order.
    std::size_t covered = 0;
    for (auto const& p : ns.patterns) {
        EXPECT_EQ(p.first, covered);
        covered += p.count;
    }
    EXPECT_EQ(covered, ns.size());

    auto const* lv = find_pattern(ns, "input 1-2 level");
    EXPECT_TRUE(lv != nullptr);
    EXPECT_EQ(lv->count, std::size_t(2));
    EXPECT_TRUE(ns.resolve_path("/input/1/level") == std::optional<controlplane::ControlPointId>(lv->first));

    auto const* eq = find_pattern(ns, "input 1-2 parametric 1-2 level");
    EXPECT_TRUE(eq != nullptr);
    EXPECT_EQ(eq->count, std::size_t(4));
    EXPECT_TRUE(ns.resolve_path("/input/1/parametric/2/level") == std::optional<controlplane::ControlPointId>(eq->first + 1));
    EXPECT_TRUE(ns.resolve_path("/input/2/parametric/1/level") == std::optional<controlplane::ControlPointId>(eq->first + 2));

    // A single-count dimension is spelled "1", not "1-1".
    auto const* xp = find_pattern(ns, "bus 1-2 output 1 delay");
    EXPECT_TRUE(xp != nullptr);
    EXPECT_EQ(xp->count, std::size_t(2));

    auto const* sys = find_pattern(ns, "system mute");
    EXPECT_TRUE(sys != nullptr);
    EXPECT_EQ(sys->count, std::size_t(1));
}

TEST(nadia_schema, scalar_projection)
{
    using K = NadiaValueKind;
    EXPECT_TRUE(nadia_scalar_of(K::decibels, -90.0, "-3.6") == std::optional<double>(-3.6));
    EXPECT_TRUE(nadia_scalar_of(K::decibels, -90.0, " -3.6 ") == std::optional<double>(-3.6));
    EXPECT_TRUE(nadia_scalar_of(K::decibels, -90.0, "-inf") == std::optional<double>(-90.0));
    EXPECT_TRUE(nadia_scalar_of(K::decibels, -110.0, "-Inf") == std::optional<double>(-110.0));
    EXPECT_TRUE(nadia_scalar_of(K::decibels, -90.0, "0 dB") == std::optional<double>(0.0));
    EXPECT_TRUE(nadia_scalar_of(K::nanoseconds, 0.0, "1500000") == std::optional<double>(1500000.0));
    EXPECT_TRUE(nadia_scalar_of(K::boolean, 0.0, "true") == std::optional<double>(1.0));
    EXPECT_TRUE(nadia_scalar_of(K::boolean, 0.0, "False") == std::optional<double>(0.0));
    EXPECT_TRUE(nadia_scalar_of(K::boolean, 0.0, "1") == std::optional<double>(1.0));
    EXPECT_FALSE(nadia_scalar_of(K::boolean, 0.0, "maybe").has_value());
    EXPECT_FALSE(nadia_scalar_of(K::text, 0.0, "Vox").has_value());
    EXPECT_FALSE(nadia_scalar_of(K::decibels, -90.0, "").has_value());
    EXPECT_FALSE(nadia_scalar_of(K::decibels, -90.0, "abc").has_value());

    EXPECT_TRUE(nadia_format_value(K::boolean, 1.0) == "true");
    EXPECT_TRUE(nadia_format_value(K::boolean, 0.0) == "false");
    EXPECT_TRUE(nadia_format_value(K::decibels, -3.6) == "-3.6");
    EXPECT_TRUE(nadia_format_value(K::decibels, 0.0) == "0");
    EXPECT_TRUE(nadia_format_value(K::nanoseconds, 1500000.0) == "1500000");
    EXPECT_TRUE(nadia_format_value(K::integer, 7.0) == "7");
    EXPECT_TRUE(nadia_format_value(K::hertz, 1000.0) == "1000");
    EXPECT_TRUE(nadia_format_value(K::ratio, 0.707) == "0.707");
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane_devices, nadia_schema_test)
