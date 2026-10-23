// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the Bitset / Selector handle-space bitset.

#include "statusbar/controlplane/controlplane_bitset.hpp"

#include "statusbar/test/test.hpp"

#include <vector>

using namespace statusbar;
using controlplane::Bitset;
using controlplane::ControlPointId;

TEST(bitset, set_test_reset)
{
    Bitset b(130);
    EXPECT_EQ(b.size(), 130u);
    EXPECT_TRUE(b.none());
    b.set(0);
    b.set(64);
    b.set(129);
    EXPECT_TRUE(b.test(0));
    EXPECT_TRUE(b.test(64));
    EXPECT_TRUE(b.test(129));
    EXPECT_FALSE(b.test(1));
    EXPECT_EQ(b.count(), 3u);
    b.reset(64);
    EXPECT_FALSE(b.test(64));
    EXPECT_EQ(b.count(), 2u);
    EXPECT_TRUE(b.any());
    b.clear();
    EXPECT_TRUE(b.none());
}

TEST(bitset, set_all_masks_tail)
{
    Bitset b(70);
    b.set_all();
    EXPECT_EQ(b.count(), 70u);
    // A word-aligned size has no tail to mask.
    Bitset w(128);
    w.set_all();
    EXPECT_EQ(w.count(), 128u);
}

TEST(bitset, resize_preserves)
{
    Bitset b(10);
    b.set(3);
    b.set(9);
    b.resize(200);
    EXPECT_EQ(b.size(), 200u);
    EXPECT_TRUE(b.test(3));
    EXPECT_TRUE(b.test(9));
    EXPECT_EQ(b.count(), 2u);
    b.resize(4);
    EXPECT_TRUE(b.test(3));
    EXPECT_EQ(b.count(), 1u);
}

TEST(bitset, bitwise_ops_and_equality)
{
    Bitset a(100), b(100);
    a.set(1);
    a.set(50);
    b.set(50);
    b.set(99);

    Bitset o = a;
    o |= b;
    EXPECT_EQ(o.count(), 3u);
    EXPECT_TRUE(o.test(1));
    EXPECT_TRUE(o.test(50));
    EXPECT_TRUE(o.test(99));

    Bitset n = a;
    n &= b;
    EXPECT_EQ(n.count(), 1u);
    EXPECT_TRUE(n.test(50));

    EXPECT_TRUE(a == a);
    EXPECT_FALSE(a == b);
}

TEST(bitset, or_and_fused)
{
    Bitset dirty(100), interest(100), accum(100);
    dirty.set(2);
    dirty.set(3);
    interest.set(3);
    interest.set(4);
    accum.set(90);
    accum.or_and(dirty, interest);
    EXPECT_EQ(accum.count(), 2u);
    EXPECT_TRUE(accum.test(3));
    EXPECT_TRUE(accum.test(90));
}

TEST(bitset, for_each_set_ascending)
{
    Bitset b(200);
    b.set(199);
    b.set(0);
    b.set(63);
    b.set(64);
    std::vector<ControlPointId> seen;
    b.for_each_set([&](ControlPointId h) { seen.push_back(h); });
    EXPECT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0], 0u);
    EXPECT_EQ(seen[1], 63u);
    EXPECT_EQ(seen[2], 64u);
    EXPECT_EQ(seen[3], 199u);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane, controlplane_bitset_test)
