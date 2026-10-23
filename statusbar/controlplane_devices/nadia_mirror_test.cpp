// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for NadiaMirror: response lines land on the right handles,
// empty values (no design) stay unpopulated, unknown names are collected.

#include "statusbar/controlplane_devices/nadia_mirror.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;
using namespace statusbar::controlplane_devices;

namespace {

NadiaMirror make_mirror()
{
    NadiaShape s;
    s.inputs = 2;
    s.buses = 1;
    s.outputs = 1;
    s.eq_bands = 1;
    return NadiaMirror(build_nadia_namespace(s));
}

}  // namespace

TEST(nadia_mirror, value_reports_land)
{
    auto m = make_mirror();
    auto level = m.ns().resolve_path("/input/1/level");
    auto mute = m.ns().resolve_path("/input/2/mute");
    auto name = m.ns().resolve_path("/input/1/name");
    EXPECT_TRUE(level && mute && name);

    EXPECT_TRUE(m.consume_line("Got (req-1): Input 1 Level = -3.6", 10) == level);
    EXPECT_TRUE(m.consume_line("Got: Input 2 Mute = true", 11) == mute);
    EXPECT_TRUE(m.consume_line("Got (x): Input 1 Name = Vox", 12) == name);

    EXPECT_TRUE(m.value_of(*level) == "-3.6");
    EXPECT_TRUE(m.scalar_of(*level) == std::optional<double>(-3.6));
    EXPECT_EQ(controlplane::value_double(m.instance().read(*level).reported), -3.6);
    EXPECT_TRUE(m.scalar_of(*mute) == std::optional<double>(1.0));
    EXPECT_TRUE(m.value_of(*name) == "Vox");
    EXPECT_FALSE(m.scalar_of(*name).has_value());

    EXPECT_TRUE(m.reported().test(*level));
    EXPECT_TRUE(m.populated().test(*level));
    EXPECT_TRUE(m.populated().test(*name));
    EXPECT_EQ(m.reported().count(), std::size_t(3));
    EXPECT_EQ(m.missing().count(), m.ns().size() - 3);
    EXPECT_EQ(m.value_report_count(), std::size_t(3));

    // "-inf" projects to the point's floor.
    m.consume_line("Got: Input 1 Level = -inf", 13);
    EXPECT_TRUE(m.scalar_of(*level) == std::optional<double>(-90.0));
}

TEST(nadia_mirror, empty_values_report_but_do_not_populate)
{
    auto m = make_mirror();
    auto level = m.ns().resolve_path("/input/1/level");
    EXPECT_TRUE(m.consume_line("Got (req-13): Input 1 Level = ", 1) == level);
    EXPECT_TRUE(m.reported().test(*level));
    EXPECT_FALSE(m.populated().test(*level));
    EXPECT_FALSE(m.scalar_of(*level).has_value());
    EXPECT_FALSE(m.missing().test(*level));

    // A later real value populates; a later empty value un-populates.
    m.consume_line("Got: Input 1 Level = 0", 2);
    EXPECT_TRUE(m.populated().test(*level));
    m.consume_line("Got: Input 1 Level =", 3);
    EXPECT_FALSE(m.populated().test(*level));
    EXPECT_TRUE(m.reported().test(*level));
}

TEST(nadia_mirror, unknown_and_other_lines)
{
    auto m = make_mirror();
    EXPECT_FALSE(m.consume_line("Got (req-18):  = unset", 1).has_value());
    EXPECT_EQ(m.unknown_address_count(), std::size_t(1));

    EXPECT_FALSE(m.consume_line("Got: Cue 3 Name = Intro", 2).has_value());
    EXPECT_FALSE(m.consume_line("Got: Cue 3 Name = Intro again", 3).has_value());
    EXPECT_FALSE(m.consume_line("Got: Input 9 Level = 0", 4).has_value());
    EXPECT_EQ(m.unknown_names().size(), std::size_t(2));
    EXPECT_TRUE(m.unknown_names()[0] == "Cue 3 Name");
    EXPECT_TRUE(m.unknown_names()[1] == "Input 9 Level");
    EXPECT_EQ(m.value_report_count(), std::size_t(3));

    EXPECT_FALSE(m.consume_line("Event: hexbytes unit 1 bytes F0", 5).has_value());
    EXPECT_FALSE(m.consume_line("", 6).has_value());
    EXPECT_EQ(m.other_count(), std::size_t(1));
    EXPECT_EQ(m.reported().count(), std::size_t(0));
}

TEST(nadia_mirror, local_set_echo)
{
    auto m = make_mirror();
    auto mute = m.ns().resolve_path("/output/1/mute");
    EXPECT_TRUE(mute.has_value());
    m.record_local_set(*mute, "true", 5);
    EXPECT_TRUE(m.value_of(*mute) == "true");
    EXPECT_TRUE(m.scalar_of(*mute) == std::optional<double>(1.0));
    EXPECT_TRUE(m.reported().test(*mute));
    m.consume_line("Got: Output 1 Mute = false", 6);
    EXPECT_TRUE(m.scalar_of(*mute) == std::optional<double>(0.0));
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane_devices, nadia_mirror_test)
