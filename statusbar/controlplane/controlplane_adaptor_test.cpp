// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for PathAdaptor: exact resolution, pattern compilation to
// Selector, address_of round trips, and builder rules.

#include "statusbar/controlplane/controlplane_adaptor.hpp"

#include "statusbar/test/test.hpp"

#include <string>

using namespace statusbar;
using namespace statusbar::controlplane;

namespace {

PathAdaptor make_small()
{
    PathAdaptorBuilder b(8);
    b.map("/input/1/gain", 0);
    b.map("/input/1/mute", 1);
    b.map("/input/2/gain", 2);
    b.map("/input/master/gain", 3);
    b.map("/output/1/gain", 4);
    b.map("/status/temperature", 5);
    return b.build();
}

}  // namespace

TEST(path_adaptor, resolve_exact)
{
    auto a = make_small();
    EXPECT_EQ(a.handle_space(), 8u);
    EXPECT_TRUE(a.resolve("/input/1/gain").has_value());
    EXPECT_EQ(*a.resolve("/input/1/gain"), 0u);
    EXPECT_EQ(*a.resolve("/input/2/gain"), 2u);
    EXPECT_EQ(*a.resolve("/status/temperature"), 5u);
    // Slash normalization: doubled and trailing slashes are cosmetic.
    EXPECT_EQ(*a.resolve("input/1//gain/"), 0u);

    EXPECT_FALSE(a.resolve("/input/3/gain").has_value());    // unknown branch
    EXPECT_FALSE(a.resolve("/input/1").has_value());         // prefix, not a leaf
    EXPECT_FALSE(a.resolve("/input/007/gain").has_value());  // "007" is not "7"
    EXPECT_FALSE(a.resolve("/bogus/1/gain").has_value());    // unseen symbol
    EXPECT_FALSE(a.resolve("").has_value());
}

TEST(path_adaptor, resolve_pattern_to_selector)
{
    auto a = make_small();

    auto s = a.resolve_pattern("/input/#/gain");
    EXPECT_EQ(s.size(), 8u);
    EXPECT_EQ(s.count(), 2u);  // handles 0 and 2; "master" is not numeric
    EXPECT_TRUE(s.test(0));
    EXPECT_TRUE(s.test(2));

    s = a.resolve_pattern("/input/*/gain");
    EXPECT_EQ(s.count(), 3u);  // master included
    EXPECT_TRUE(s.test(3));

    s = a.resolve_pattern("/*/1/gain");
    EXPECT_EQ(s.count(), 2u);  // input and output

    // Selectors compose bitwise.
    auto combined = a.resolve_pattern("/input/#/mute");
    combined |= a.resolve_pattern("/status/temperature");
    EXPECT_EQ(combined.count(), 2u);
    EXPECT_TRUE(combined.test(1));
    EXPECT_TRUE(combined.test(5));

    // Unseen symbols match nothing, quietly.
    EXPECT_EQ(a.resolve_pattern("/bogus/*").count(), 0u);
    EXPECT_EQ(a.resolve_pattern("").count(), 0u);
}

TEST(path_adaptor, address_of_round_trips)
{
    auto a = make_small();
    for (ControlPointId h = 0; h < 6; ++h) {
        auto const& path = a.address_of(h);
        EXPECT_FALSE(path.empty());
        EXPECT_TRUE(a.resolve(path).has_value());
        EXPECT_EQ(*a.resolve(path), h);
    }
    // Unmapped handles have no address.
    EXPECT_TRUE(a.address_of(7).empty());
}

TEST(path_adaptor, builder_rules)
{
    PathAdaptorBuilder b(4);
    EXPECT_TRUE(b.map("/a/1", 0));
    EXPECT_FALSE(b.map("/a/1", 1));  // duplicate path
    EXPECT_FALSE(b.map("/a/2", 0));  // handle already mapped
    EXPECT_FALSE(b.map("/a/3", 4));  // handle out of range
    EXPECT_FALSE(b.map("///", 1));   // no segments
    EXPECT_TRUE(b.map("/a/2", 1));

    auto a = b.build();
    EXPECT_EQ(*a.resolve("/a/1"), 0u);
    EXPECT_EQ(*a.resolve("/a/2"), 1u);

    // build() resets the builder for the same handle space.
    EXPECT_TRUE(b.map("/a/1", 0));
    auto a2 = b.build();
    EXPECT_EQ(*a2.resolve("/a/1"), 0u);
    EXPECT_TRUE(a2.address_of(1).empty());
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane, controlplane_adaptor_test)
