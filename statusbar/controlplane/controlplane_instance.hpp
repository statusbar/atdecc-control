// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane_bitset.hpp"
#include "statusbar/controlplane/controlplane_model.hpp"

#include <cstdint>
#include <memory>
#include <vector>

// The dynamic, per-device half of the control plane (docs/PLAN.md §4.2,
// §4.5, §4.6). Threading contract: exactly one owner thread calls the write
// API, subscribe/unsubscribe, hold/resume and tick(); any thread may call
// the read API, which uses a per-handle seqlock (version even = stable, odd
// = writer active) and never blocks the writer.
//
// Entity vs controller is the only behavioural difference: in an entity a
// local set is authoritative (desired and reported move together); in a
// controller or the proxy a local set writes desired and marks pending, the
// device response writes reported, and a timeout or error clears pending and
// leaves reported alone, so the UI snaps back to truth.

namespace statusbar::controlplane {

enum class Role : std::uint8_t
{
    entity,
    controller,
};

/// Per-subscriber fan-out timing (docs/PLAN.md §4.5). Deliberately renamed
/// from the original ControlPlane fields, which meant the opposite of what
/// they said. Defaults are the GALAXY values.
struct Subscription
{
    Selector interest;
    std::uint32_t throttle_ms = 30;     ///< minimum interval between change notifications
    std::uint32_t refresh_ms = 10'000;  ///< maximum staleness; forces a full interest-set resend
};

using SubscriptionId = std::uint32_t;

class Instance
{
  public:
    Instance(std::shared_ptr<Model const> model, Role role);

    Model const& model() const noexcept { return *model_; }
    Role role() const noexcept { return role_; }
    std::size_t size() const noexcept { return model_->size(); }

    // ---- owner-thread write API -------------------------------------

    /// A locally-requested value change. Entity: authoritative — desired and
    /// reported move together and confirmed_at is set. Controller: writes
    /// desired only and marks the point pending until the device answers.
    void set_local(ControlPointId h, std::uint64_t bits, std::uint64_t now_ns);

    /// The device-reported value (a response or an unsolicited
    /// notification). Authoritative for reported; clears pending.
    void set_reported(ControlPointId h, std::uint64_t bits, std::uint64_t now_ns);

    /// A timed-out or rejected local set: clears pending and leaves reported
    /// alone, so readers snap back to truth. Desired snaps back to reported.
    void fail_pending(ControlPointId h);

    // ---- any-thread seqlock reads -----------------------------------

    struct Snapshot
    {
        std::uint64_t reported = 0;
        std::uint64_t desired = 0;
        std::uint64_t confirmed_at_ns = 0;
    };

    /// Coherent per-handle snapshot; retries while a write is in flight.
    Snapshot read(ControlPointId h) const;

    std::uint64_t read_reported(ControlPointId h) const { return read(h).reported; }
    std::uint64_t read_desired(ControlPointId h) const { return read(h).desired; }

    // ---- owner-thread state -----------------------------------------

    Bitset const& pending() const noexcept { return pending_; }
    Bitset const& dirty() const noexcept { return dirty_; }

    // ---- subscriptions + change fan-out -----------------------------

    /// Registers a subscriber; now_ns baselines its refresh clock. The
    /// interest selector must match size().
    SubscriptionId subscribe(Subscription sub, std::uint64_t now_ns);
    void unsubscribe(SubscriptionId id);

    /// Batched updates suppress tick() for their duration (docs/PLAN.md
    /// §4.6); nested holds are counted.
    void hold() noexcept { ++hold_count_; }
    void resume() noexcept { --hold_count_; }

    class ScopedHold
    {
      public:
        explicit ScopedHold(Instance& i) noexcept
            : instance_(i)
        {
            instance_.hold();
        }
        ~ScopedHold() { instance_.resume(); }
        ScopedHold(ScopedHold const&) = delete;
        ScopedHold& operator=(ScopedHold const&) = delete;

      private:
        Instance& instance_;
    };

    /// Distributes dirty bits to every subscriber's accumulator (dirty &
    /// interest), then emits per subscriber under its throttle/refresh
    /// timing. Emit is called as emit(SubscriptionId, const Selector &)
    /// where the selector holds the changed (or, on refresh, the full
    /// interest) handle set. No-op while held.
    template <typename Emit>
    void tick(std::uint64_t now_ns, Emit&& emit)
    {
        if (hold_count_ > 0) {
            return;
        }
        distribute_dirty();
        for (SubscriptionId id = 0; id < SubscriptionId(subs_.size()); ++id) {
            auto& slot = subs_[id];
            if (!slot.alive || slot.sub.interest.none()) {
                continue;
            }
            auto refresh_ns = std::uint64_t(slot.sub.refresh_ms) * 1'000'000u;
            auto throttle_ns = std::uint64_t(slot.sub.throttle_ms) * 1'000'000u;
            if (now_ns - slot.last_refresh_ns >= refresh_ns) {
                emit(id, slot.sub.interest);
                slot.accum.clear();
                slot.last_refresh_ns = now_ns;
                slot.last_emit_ns = now_ns;
            } else if (slot.accum.any() && (slot.last_emit_ns == 0 || now_ns - slot.last_emit_ns >= throttle_ns)) {
                emit(id, slot.accum);
                slot.accum.clear();
                slot.last_emit_ns = now_ns;
            }
        }
    }

  private:
    struct SubSlot
    {
        Subscription sub;
        Bitset accum;
        std::uint64_t last_emit_ns = 0;
        std::uint64_t last_refresh_ns = 0;
        bool alive = false;
    };

    void distribute_dirty();

    void begin_write(ControlPointId h) noexcept;
    void end_write(ControlPointId h) noexcept;

    std::shared_ptr<Model const> model_;
    Role role_;

    std::vector<std::uint64_t> reported_;
    std::vector<std::uint64_t> desired_;
    std::vector<std::uint32_t> version_;  ///< seqlock: even = stable, odd = writer active
    std::vector<std::uint64_t> confirmed_at_ns_;
    Bitset pending_;  ///< desired != reported, in flight
    Bitset dirty_;    ///< changed since last tick

    std::vector<SubSlot> subs_;
    std::uint32_t hold_count_ = 0;
};

}  // namespace statusbar::controlplane
