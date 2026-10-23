// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for Model / ModelBuilder.

#include "statusbar/controlplane/controlplane_model.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;
using namespace statusbar::controlplane;

TEST(model_builder, handles_are_dense_and_ordered)
{
    ModelBuilder b;
    ControlPointDesc d;
    d.control_type = 0x001cab0000400001ull;
    d.min = -90.0;
    d.max = 10.0;
    d.step = 0.1;
    d.def = 0.0;
    auto h0 = b.add(d);
    d.flags = PointFlags::read_only;
    auto h1 = b.add(d);
    d.flags = PointFlags::read_only | PointFlags::volatile_value;
    d.aem = AemRef{0, 0x001a, 7, 2};
    auto h2 = b.add(d);

    EXPECT_EQ(h0, 0u);
    EXPECT_EQ(h1, 1u);
    EXPECT_EQ(h2, 2u);
    EXPECT_EQ(b.size(), 3u);

    auto m = b.build();
    EXPECT_EQ(m->size(), 3u);
    EXPECT_EQ(m->control_type(0), 0x001cab0000400001ull);
    EXPECT_EQ(m->min(0), -90.0);
    EXPECT_EQ(m->max(0), 10.0);
    EXPECT_FALSE(m->read_only(0));
    EXPECT_TRUE(m->read_only(1));
    EXPECT_TRUE(has_flag(m->flags(2), PointFlags::volatile_value));
    EXPECT_TRUE(m->aem(2) == (AemRef{0, 0x001a, 7, 2}));
    EXPECT_EQ(m->aem(0).descriptor_type, AemRef::NO_DESCRIPTOR);
    EXPECT_EQ(m->localized(0), ControlPointDesc::NO_LOCALIZED);
}

TEST(model_builder, build_resets_builder)
{
    ModelBuilder b;
    b.add(ControlPointDesc{});
    auto m1 = b.build();
    EXPECT_EQ(m1->size(), 1u);
    EXPECT_EQ(b.size(), 0u);
    auto m2 = b.build();
    EXPECT_EQ(m2->size(), 0u);
    // The first model is unaffected by the builder's reuse.
    EXPECT_EQ(m1->size(), 1u);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane, controlplane_model_test)
