// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for GalaxyMirror: value reports feed the Instance and string
// store; reconciliation tracks reported/missing/unknown.

#include "statusbar/controlplane_devices/galaxy_mirror.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;
using namespace statusbar::controlplane;
using namespace statusbar::controlplane_devices;

TEST(galaxy_mirror, value_reports_update_state)
{
    GalaxyMirror m;
    auto h = m.consume_line("/processing/input/1/gain='-6.5'", 1000);
    EXPECT_TRUE(h.has_value());
    EXPECT_TRUE(m.value_of(*h) == "-6.5");
    EXPECT_TRUE(m.reported().test(*h));
    EXPECT_EQ(value_double(m.instance().read(*h).reported), -6.5);
    EXPECT_EQ(m.instance().read(*h).confirmed_at_ns, 1000u);

    // Booleans map to 1/0 in the Instance.
    auto mute = m.consume_line("/processing/input/1/mute='true'", 2000);
    EXPECT_TRUE(mute.has_value());
    EXPECT_EQ(value_double(m.instance().read(*mute).reported), 1.0);

    // Non-scalar values live in the string store only.
    auto name = m.consume_line("/device/input/1/name='Vocal L'", 3000);
    EXPECT_TRUE(name.has_value());
    EXPECT_TRUE(m.value_of(*name) == "Vocal L");

    // Alias-form reports land on the same handle as the path form.
    auto via_alias = m.consume_line("@371='-3'", 4000);
    EXPECT_TRUE(via_alias.has_value());
    EXPECT_EQ(*via_alias, *h);
    EXPECT_TRUE(m.value_of(*h) == "-3");

    EXPECT_EQ(m.value_report_count(), 4u);
    EXPECT_EQ(m.reported().count(), 3u);
}

TEST(galaxy_mirror, reconciliation)
{
    GalaxyMirror m;
    EXPECT_EQ(m.missing().count(), m.ns().size());

    m.consume_line("/processing/input/1/mute='false'", 1);
    EXPECT_EQ(m.missing().count(), m.ns().size() - 1);

    // A path the schema does not know is collected once, not counted as
    // reported.
    EXPECT_FALSE(m.consume_line("/processing/input/1/new_feature='1'", 2).has_value());
    m.consume_line("/processing/input/1/new_feature='2'", 3);
    EXPECT_EQ(m.unknown_paths().size(), 1u);
    EXPECT_TRUE(m.unknown_paths()[0] == "/processing/input/1/new_feature");
    EXPECT_EQ(m.reported().count(), 1u);
    EXPECT_EQ(m.value_report_count(), 3u);

    // Describe and noise lines are counted, never treated as values.
    m.consume_line("?{'/a': { 'x' : '1'}}", 4);
    m.consume_line("welcome to GALAXY", 5);
    EXPECT_EQ(m.describe_count(), 1u);
    EXPECT_EQ(m.other_count(), 1u);
    EXPECT_EQ(m.reported().count(), 1u);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane_devices, galaxy_mirror_test)
