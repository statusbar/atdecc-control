// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for Subscription and the tick() change fan-out: interest
// masking, throttle, refresh, hold/resume.

#include "statusbar/controlplane/controlplane.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <memory>
#include <vector>

using namespace statusbar;
using namespace statusbar::controlplane;

namespace {

constexpr std::uint64_t MS = 1'000'000ull;
constexpr std::uint64_t T0 = 1'000'000'000ull;  // an arbitrary epoch-ish base

std::shared_ptr<Model const> make_model(std::size_t n)
{
    ModelBuilder b;
    for (std::size_t i = 0; i < n; ++i) {
        b.add(ControlPointDesc{});
    }
    return b.build();
}

struct Emitted
{
    SubscriptionId id;
    std::vector<ControlPointId> handles;
};

struct Collector
{
    std::vector<Emitted> emits;

    void operator()(SubscriptionId id, Selector const& changed)
    {
        Emitted e;
        e.id = id;
        changed.for_each_set([&](ControlPointId h) { e.handles.push_back(h); });
        emits.push_back(std::move(e));
    }
};

Subscription sub_for(
    std::size_t model_size, std::initializer_list<ControlPointId> handles, std::uint32_t throttle_ms, std::uint32_t refresh_ms)
{
    Subscription s;
    s.interest = Selector(model_size);
    for (auto h : handles) {
        s.interest.set(h);
    }
    s.throttle_ms = throttle_ms;
    s.refresh_ms = refresh_ms;
    return s;
}

}  // namespace

TEST(tick, interest_masks_changes)
{
    Instance inst(make_model(8), Role::entity);
    auto id = inst.subscribe(sub_for(8, {1, 5}, 0, 60'000), T0);

    inst.set_local(1, 10, T0);
    inst.set_local(2, 20, T0);  // outside interest

    Collector c;
    inst.tick(T0 + 1 * MS, c);
    EXPECT_EQ(c.emits.size(), 1u);
    EXPECT_EQ(c.emits[0].id, id);
    EXPECT_EQ(c.emits[0].handles.size(), 1u);
    EXPECT_EQ(c.emits[0].handles[0], 1u);

    // Nothing new: no emission.
    inst.tick(T0 + 2 * MS, c);
    EXPECT_EQ(c.emits.size(), 1u);
}

TEST(tick, multiple_subscribers_get_their_own_slices)
{
    Instance inst(make_model(8), Role::entity);
    auto a = inst.subscribe(sub_for(8, {0, 1}, 0, 60'000), T0);
    auto b = inst.subscribe(sub_for(8, {1, 2}, 0, 60'000), T0);

    inst.set_local(1, 11, T0);
    inst.set_local(2, 22, T0);

    Collector c;
    inst.tick(T0 + 1 * MS, c);
    EXPECT_EQ(c.emits.size(), 2u);
    EXPECT_EQ(c.emits[0].id, a);
    EXPECT_EQ(c.emits[0].handles.size(), 1u);  // handle 1 only
    EXPECT_EQ(c.emits[0].handles[0], 1u);
    EXPECT_EQ(c.emits[1].id, b);
    EXPECT_EQ(c.emits[1].handles.size(), 2u);  // handles 1 and 2
}

TEST(tick, throttle_accumulates_and_flushes)
{
    Instance inst(make_model(8), Role::entity);
    inst.subscribe(sub_for(8, {0, 1, 2}, 30, 60'000), T0);

    Collector c;
    // First change emits immediately (no prior notification).
    inst.set_local(0, 1, T0);
    inst.tick(T0 + 1 * MS, c);
    EXPECT_EQ(c.emits.size(), 1u);

    // Changes inside the throttle window accumulate silently.
    inst.set_local(1, 1, T0 + 5 * MS);
    inst.tick(T0 + 10 * MS, c);
    inst.set_local(2, 1, T0 + 15 * MS);
    inst.tick(T0 + 20 * MS, c);
    EXPECT_EQ(c.emits.size(), 1u);

    // Window over: one coalesced notification with both handles.
    inst.tick(T0 + 32 * MS, c);
    EXPECT_EQ(c.emits.size(), 2u);
    EXPECT_EQ(c.emits[1].handles.size(), 2u);
    EXPECT_EQ(c.emits[1].handles[0], 1u);
    EXPECT_EQ(c.emits[1].handles[1], 2u);
}

TEST(tick, refresh_resends_full_interest)
{
    Instance inst(make_model(8), Role::entity);
    inst.subscribe(sub_for(8, {3, 4}, 30, 10'000), T0);

    Collector c;
    // No changes at all — the refresh interval still forces a full resend.
    inst.tick(T0 + 9'999 * MS, c);
    EXPECT_EQ(c.emits.size(), 0u);
    inst.tick(T0 + 10'000 * MS, c);
    EXPECT_EQ(c.emits.size(), 1u);
    EXPECT_EQ(c.emits[0].handles.size(), 2u);
    EXPECT_EQ(c.emits[0].handles[0], 3u);
    EXPECT_EQ(c.emits[0].handles[1], 4u);

    // The refresh clock rebaselines; the next one is another interval out.
    inst.tick(T0 + 10'001 * MS, c);
    EXPECT_EQ(c.emits.size(), 1u);
}

TEST(tick, hold_suppresses_fanout)
{
    Instance inst(make_model(4), Role::entity);
    inst.subscribe(sub_for(4, {0, 1}, 0, 60'000), T0);

    Collector c;
    {
        Instance::ScopedHold guard(inst);
        inst.set_local(0, 1, T0);
        inst.tick(T0 + 1 * MS, c);
        inst.set_local(1, 2, T0 + 1 * MS);
        inst.tick(T0 + 2 * MS, c);
        EXPECT_EQ(c.emits.size(), 0u);
    }
    // Released: the batch flushes as one coalesced notification.
    inst.tick(T0 + 3 * MS, c);
    EXPECT_EQ(c.emits.size(), 1u);
    EXPECT_EQ(c.emits[0].handles.size(), 2u);
}

TEST(tick, unsubscribe_stops_emission)
{
    Instance inst(make_model(4), Role::entity);
    auto id = inst.subscribe(sub_for(4, {0}, 0, 60'000), T0);

    inst.set_local(0, 1, T0);
    Collector c;
    inst.tick(T0 + 1 * MS, c);
    EXPECT_EQ(c.emits.size(), 1u);

    inst.unsubscribe(id);
    inst.set_local(0, 2, T0 + 2 * MS);
    inst.tick(T0 + 3 * MS, c);
    EXPECT_EQ(c.emits.size(), 1u);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane, controlplane_tick_test)
