// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the Nadia CP text-command codec: address spellings,
// command formatting, and Got: line parsing (shapes captured from the LA
// Nadia on 2026-09-01).

#include "statusbar/controlplane_devices/nadia_ascii.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;
using namespace statusbar::controlplane_devices;

TEST(nadia_ascii, address_spellings)
{
    EXPECT_TRUE(nadia_command_address("/input/1/level") == "input 1 level");
    EXPECT_TRUE(nadia_command_address("/bus/6/output/4/level") == "bus 6 output 4 level");
    EXPECT_TRUE(nadia_command_address("/system/mute") == "system mute");

    EXPECT_TRUE(nadia_friendly_name("/input/1/level") == "Input 1 Level");
    EXPECT_TRUE(nadia_friendly_name("/bus/6/output/4/level") == "Bus 6 Output 4 Level");
    // Catalogue spellings survive the round trip.
    EXPECT_TRUE(nadia_friendly_name("/vgroup/2/mute") == "VGroup 2 Mute");
    EXPECT_TRUE(nadia_friendly_name("/input/1/eq/bypass") == "Input 1 EQ Bypass");
    EXPECT_TRUE(nadia_friendly_name("/input/1/dynamics/noisegate/threshold") == "Input 1 Dynamics NoiseGate Threshold");
    EXPECT_TRUE(nadia_friendly_name("/input/3/parametric/2/bandbypass") == "Input 3 Parametric 2 BandBypass");

    EXPECT_TRUE(nadia_path_of_name("Input 1 Level") == "/input/1/level");
    EXPECT_TRUE(nadia_path_of_name("Bus 6 Output 4 Level") == "/bus/6/output/4/level");
    EXPECT_TRUE(nadia_path_of_name("  Input   1  Level ") == "/input/1/level");

    // The matching key ignores case, spaces and slashes.
    EXPECT_TRUE(nadia_name_key("Input 1 Level") == "input1level");
    EXPECT_TRUE(nadia_name_key("/input/1/level") == "input1level");
    EXPECT_TRUE(nadia_name_key("Input 1 Dynamics Noise Gate Threshold") == nadia_name_key("/input/1/dynamics/noisegate/threshold"));
}

TEST(nadia_ascii, command_formatting)
{
    EXPECT_TRUE(nadia_get("input 1-8 level") == "get input 1-8 level\n");
    EXPECT_TRUE(nadia_get("input 1 level", "foo") == "get foo = input 1 level\n");
    EXPECT_TRUE(nadia_set("output 5 level", "-3.6") == "set output 5 level = -3.6\n");
    EXPECT_TRUE(nadia_set("output 5,7,9-11 mute", "false") == "set output 5,7,9-11 mute = false\n");
    EXPECT_TRUE(nadia_subscribe("input 1-2 level") == "subscribe input 1-2 level\n");
    EXPECT_TRUE(nadia_unsubscribe("input 1-2 level") == "unsubscribe input 1-2 level\n");
    EXPECT_TRUE(nadia_unsubscribe_all() == "unsubscribeall\n");
}

TEST(nadia_ascii, parse_value_lines)
{
    // Tagged get reply (auto tag).
    auto p = parse_nadia_line("Got (req-13): Input 1 Level = -3.6");
    EXPECT_TRUE(p.kind == NadiaLineKind::value);
    EXPECT_TRUE(p.tag == "req-13");
    EXPECT_TRUE(p.name == "Input 1 Level");
    EXPECT_TRUE(p.value == "-3.6");
    EXPECT_FALSE(p.unknown_address);

    // User tag.
    p = parse_nadia_line("Got (foo): Input 1 Level = -inf");
    EXPECT_TRUE(p.tag == "foo");
    EXPECT_TRUE(p.value == "-inf");

    // Subscription push: no tag.
    p = parse_nadia_line("Got: Input 2 Level = -inf");
    EXPECT_TRUE(p.kind == NadiaLineKind::value);
    EXPECT_TRUE(p.tag.empty());
    EXPECT_TRUE(p.name == "Input 2 Level");
    EXPECT_TRUE(p.value == "-inf");

    // No design loaded: the value is empty, with or without the trailing space.
    p = parse_nadia_line("Got (req-13): Input 1 Level = ");
    EXPECT_TRUE(p.kind == NadiaLineKind::value);
    EXPECT_TRUE(p.name == "Input 1 Level");
    EXPECT_TRUE(p.value.empty());
    p = parse_nadia_line("Got (req-13): Input 1 Level =");
    EXPECT_TRUE(p.kind == NadiaLineKind::value);
    EXPECT_TRUE(p.name == "Input 1 Level");
    EXPECT_TRUE(p.value.empty());

    // Unknown address.
    p = parse_nadia_line("Got (req-18):  = unset");
    EXPECT_TRUE(p.kind == NadiaLineKind::value);
    EXPECT_TRUE(p.name.empty());
    EXPECT_TRUE(p.unknown_address);

    // A text value keeps everything after the first " = ", including '='.
    p = parse_nadia_line("Got: Input 1 Name = Vox = lead");
    EXPECT_TRUE(p.name == "Input 1 Name");
    EXPECT_TRUE(p.value == "Vox = lead");
}

TEST(nadia_ascii, parse_other_kinds)
{
    EXPECT_TRUE(parse_nadia_line("").kind == NadiaLineKind::empty);
    EXPECT_TRUE(parse_nadia_line("Event: hexbytes unit 1 bytes F0").kind == NadiaLineKind::other);
    EXPECT_TRUE(parse_nadia_line("[server] said: hello").kind == NadiaLineKind::other);
    EXPECT_TRUE(parse_nadia_line("Got (req-1").kind == NadiaLineKind::other);
    EXPECT_TRUE(parse_nadia_line("Gotcha").kind == NadiaLineKind::other);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane_devices, nadia_ascii_test)
