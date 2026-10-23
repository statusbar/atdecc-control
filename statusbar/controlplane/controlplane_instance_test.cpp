// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for Instance: role semantics and seqlock reads.

#include "statusbar/controlplane/controlplane.hpp"
#include "statusbar/test/test.hpp"

#include <atomic>
#include <memory>
#include <thread>

using namespace statusbar;
using namespace statusbar::controlplane;

namespace {

std::shared_ptr<Model const> make_model(std::size_t n)
{
    ModelBuilder b;
    for (std::size_t i = 0; i < n; ++i) {
        b.add(ControlPointDesc{});
    }
    return b.build();
}

}  // namespace

TEST(instance, entity_local_set_is_authoritative)
{
    Instance inst(make_model(4), Role::entity);
    inst.set_local(2, value_bits(-6.0), 1'000);
    auto s = inst.read(2);
    EXPECT_EQ(value_double(s.reported), -6.0);
    EXPECT_EQ(value_double(s.desired), -6.0);
    EXPECT_EQ(s.confirmed_at_ns, 1'000u);
    EXPECT_FALSE(inst.pending().test(2));
    EXPECT_TRUE(inst.dirty().test(2));
}

TEST(instance, controller_local_set_is_pending_until_reported)
{
    Instance inst(make_model(4), Role::controller);
    inst.set_reported(1, value_bits(0.0), 500);
    inst.set_local(1, value_bits(-6.0), 1'000);

    auto s = inst.read(1);
    EXPECT_EQ(value_double(s.reported), 0.0);  // device truth untouched
    EXPECT_EQ(value_double(s.desired), -6.0);
    EXPECT_TRUE(inst.pending().test(1));

    // Device echoes the set.
    inst.set_reported(1, value_bits(-6.0), 2'000);
    s = inst.read(1);
    EXPECT_EQ(value_double(s.reported), -6.0);
    EXPECT_EQ(value_double(s.desired), -6.0);
    EXPECT_EQ(s.confirmed_at_ns, 2'000u);
    EXPECT_FALSE(inst.pending().test(1));
}

TEST(instance, fail_pending_snaps_back_to_truth)
{
    Instance inst(make_model(4), Role::controller);
    inst.set_reported(3, value_bits(0.0), 500);
    inst.set_local(3, value_bits(-20.0), 1'000);
    EXPECT_TRUE(inst.pending().test(3));

    inst.fail_pending(3);
    auto s = inst.read(3);
    EXPECT_EQ(value_double(s.reported), 0.0);
    EXPECT_EQ(value_double(s.desired), 0.0);  // snapped back
    EXPECT_FALSE(inst.pending().test(3));

    // fail_pending on a non-pending point is a no-op.
    inst.fail_pending(2);
    EXPECT_FALSE(inst.pending().test(2));
}

TEST(instance, reported_dirty_only_on_change)
{
    Instance inst(make_model(2), Role::controller);
    inst.set_reported(0, 42, 100);
    EXPECT_TRUE(inst.dirty().test(0));
    inst.tick(1'000'000'000ull, [](SubscriptionId, Selector const&) {});
    EXPECT_FALSE(inst.dirty().test(0));
    // Same value again: not dirty.
    inst.set_reported(0, 42, 200);
    EXPECT_FALSE(inst.dirty().test(0));
}

TEST(instance, seqlock_reads_are_coherent_under_writes)
{
    // The writer keeps reported and confirmed_at in lockstep; a torn read
    // would surface as a mismatched pair.
    Instance inst(make_model(1), Role::entity);
    std::atomic<bool> stop{false};

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto s = inst.read(0);
            EXPECT_EQ(s.reported, s.confirmed_at_ns);
            EXPECT_EQ(s.desired, s.reported);
        }
    });

    for (std::uint64_t i = 1; i <= 200'000; ++i) {
        inst.set_local(0, i, i);
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    auto s = inst.read(0);
    EXPECT_EQ(s.reported, 200'000u);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane, controlplane_instance_test)
