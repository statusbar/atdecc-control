// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the GALAXY ASCII codec: command formatting, response
// parsing, and line framing.

#include "statusbar/controlplane_devices/galaxy_ascii.hpp"

#include "statusbar/test/test.hpp"

#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::controlplane_devices;

TEST(galaxy_ascii, command_formatting)
{
    EXPECT_TRUE(galaxy_get("/processing/input/1/mute") == "/processing/input/1/mute\n");
    EXPECT_TRUE(galaxy_set("/processing/input/1/mute", "true") == "/processing/input/1/mute='true'\n");
    EXPECT_TRUE(galaxy_describe("/processing/input/1/mute") == "?/processing/input/1/mute\n");
    EXPECT_TRUE(galaxy_subscribe("/processing/input/1/mute", 100) == "+/processing/input/1/mute 100\n");
    EXPECT_TRUE(galaxy_unsubscribe("/processing/input/1/mute") == "-/processing/input/1/mute\n");
    // Server-side regex patterns pass through untouched.
    EXPECT_TRUE(galaxy_get("/processing/input/\\d/mute") == "/processing/input/\\d/mute\n");
}

TEST(galaxy_ascii, parse_value_lines)
{
    auto p = parse_galaxy_line("/processing/input/1/mute='false'");
    EXPECT_TRUE(p.kind == GalaxyLineKind::value);
    EXPECT_TRUE(p.path == "/processing/input/1/mute");
    EXPECT_TRUE(p.value == "false");

    // Bare (unquoted) values and empty values.
    p = parse_galaxy_line("/processing/input/1/gain=-6.5");
    EXPECT_TRUE(p.kind == GalaxyLineKind::value);
    EXPECT_TRUE(p.value == "-6.5");
    p = parse_galaxy_line("/project/snapshot/1/name=''");
    EXPECT_TRUE(p.kind == GalaxyLineKind::value);
    EXPECT_TRUE(p.value.empty());

    // Interior quotes are data; only the outer pair is stripped.
    p = parse_galaxy_line("/status/log_message='TIME=\"2019\" ERR_MJR=0'");
    EXPECT_TRUE(p.kind == GalaxyLineKind::value);
    EXPECT_TRUE(p.value == "TIME=\"2019\" ERR_MJR=0");

    // A value containing '=' splits at the first '='.
    p = parse_galaxy_line("/a/b='x=y'");
    EXPECT_TRUE(p.path == "/a/b");
    EXPECT_TRUE(p.value == "x=y");

    // The alias address form parses like a path.
    p = parse_galaxy_line("@371='-6.5'");
    EXPECT_TRUE(p.kind == GalaxyLineKind::value);
    EXPECT_TRUE(p.path == "@371");
    EXPECT_TRUE(p.value == "-6.5");
}

TEST(galaxy_ascii, parse_other_kinds)
{
    EXPECT_TRUE(parse_galaxy_line("").kind == GalaxyLineKind::empty);
    EXPECT_TRUE(parse_galaxy_line("?{'/a': { 'description' : 'A'}}").kind == GalaxyLineKind::describe);
    EXPECT_TRUE(parse_galaxy_line("ERROR: no such control point").kind == GalaxyLineKind::other);
    // A path with no '=' is a query echo or fragment, not a value report.
    EXPECT_TRUE(parse_galaxy_line("/processing/input/1/mute").kind == GalaxyLineKind::other);
}

TEST(galaxy_ascii, line_buffer_reassembles_chunks)
{
    GalaxyLineBuffer buf;
    std::vector<std::string> lines;
    auto visit = [&](std::string_view l) { lines.emplace_back(l); };

    buf.feed("/a='1'\r\n/b=", visit);
    EXPECT_EQ(lines.size(), 1u);
    EXPECT_EQ(buf.pending(), 3u);
    buf.feed("'2'\n", visit);
    EXPECT_EQ(lines.size(), 2u);
    EXPECT_TRUE(lines[1] == "/b='2'");
    // Bare CR terminators and blank lines.
    buf.feed("\r\r/c='3'\r", visit);
    EXPECT_EQ(lines.size(), 3u);
    EXPECT_TRUE(lines[2] == "/c='3'");
    EXPECT_EQ(buf.pending(), 0u);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane_devices, galaxy_ascii_test)
