// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for AemBinding: forward/reverse resolution and per-descriptor
// selectors.

#include "statusbar/controlplane/controlplane_aem_binding.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;
using namespace statusbar::controlplane;

TEST(aem_binding, forward_reverse_and_rules)
{
    AemBindingBuilder b(8);
    EXPECT_TRUE(b.bind(AemRef{0, 0x001A, 3, 0}, 5));
    EXPECT_TRUE(b.bind(AemRef{0, 0x001A, 3, 1}, 6));
    EXPECT_TRUE(b.bind(AemRef{0, 0x0025, 0, 0}, 2));
    EXPECT_FALSE(b.bind(AemRef{0, 0x001A, 3, 0}, 7));  // ref taken
    EXPECT_FALSE(b.bind(AemRef{0, 0x001A, 4, 0}, 5));  // handle taken
    EXPECT_FALSE(b.bind(AemRef{0, 0x001A, 5, 0}, 8));  // out of range

    auto binding = b.build();
    EXPECT_EQ(binding.bound_count(), 3u);
    EXPECT_EQ(binding.handle_space(), 8u);

    EXPECT_TRUE(binding.resolve(AemRef{0, 0x001A, 3, 1}).has_value());
    EXPECT_EQ(*binding.resolve(AemRef{0, 0x001A, 3, 1}), 6u);
    EXPECT_FALSE(binding.resolve(AemRef{0, 0x001A, 9, 0}).has_value());

    EXPECT_TRUE(binding.address_of(6) == (AemRef{0, 0x001A, 3, 1}));
    EXPECT_EQ(binding.address_of(0).descriptor_type, AemRef::NO_DESCRIPTOR);

    auto sel = binding.values_of(0x001A, 3);
    EXPECT_EQ(sel.count(), 2u);
    EXPECT_TRUE(sel.test(5));
    EXPECT_TRUE(sel.test(6));
    EXPECT_EQ(binding.values_of(0x001A, 4).count(), 0u);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane, controlplane_aem_binding_test)
