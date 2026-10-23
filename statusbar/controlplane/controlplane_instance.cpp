// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane/controlplane_instance.hpp"

#include <atomic>

namespace statusbar::controlplane {

namespace {

// The seqlock data fields are plain vectors; all cross-thread accesses go
// through atomic_ref so the racing loads/stores are defined behaviour. The
// version stores carry the ordering (see begin_write/end_write and read).

inline void store_relaxed(std::uint64_t& slot, std::uint64_t v) noexcept
{
    std::atomic_ref<std::uint64_t>(slot).store(v, std::memory_order_relaxed);
}

inline std::uint64_t load_relaxed(std::uint64_t const& slot) noexcept
{
    return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(slot)).load(std::memory_order_relaxed);
}

}  // namespace

Instance::Instance(std::shared_ptr<Model const> model, Role role)
    : model_(std::move(model))
    , role_(role)
    , reported_(model_->size(), 0)
    , desired_(model_->size(), 0)
    , version_(model_->size(), 0)
    , confirmed_at_ns_(model_->size(), 0)
    , pending_(model_->size())
    , dirty_(model_->size())
{}

void Instance::begin_write(ControlPointId h) noexcept
{
    std::atomic_ref<std::uint32_t> ver(version_[h]);
    ver.store(ver.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

void Instance::end_write(ControlPointId h) noexcept
{
    std::atomic_ref<std::uint32_t> ver(version_[h]);
    ver.store(ver.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void Instance::set_local(ControlPointId h, std::uint64_t bits, std::uint64_t now_ns)
{
    begin_write(h);
    store_relaxed(desired_[h], bits);
    if (role_ == Role::entity) {
        // A local set is authoritative: desired and reported move together.
        store_relaxed(reported_[h], bits);
        store_relaxed(confirmed_at_ns_[h], now_ns);
    }
    end_write(h);
    if (role_ == Role::controller) {
        pending_.set(h);
    }
    dirty_.set(h);
}

void Instance::set_reported(ControlPointId h, std::uint64_t bits, std::uint64_t now_ns)
{
    bool was_pending = pending_.test(h);
    bool changed = load_relaxed(reported_[h]) != bits;
    begin_write(h);
    store_relaxed(reported_[h], bits);
    store_relaxed(confirmed_at_ns_[h], now_ns);
    if (was_pending) {
        // The device answered; its value is the truth from here on.
        store_relaxed(desired_[h], bits);
    }
    end_write(h);
    pending_.reset(h);
    if (changed || was_pending) {
        dirty_.set(h);
    }
}

void Instance::fail_pending(ControlPointId h)
{
    if (!pending_.test(h)) {
        return;
    }
    begin_write(h);
    // Snap desired back to the last reported truth.
    store_relaxed(desired_[h], load_relaxed(reported_[h]));
    end_write(h);
    pending_.reset(h);
    dirty_.set(h);
}

Instance::Snapshot Instance::read(ControlPointId h) const
{
    std::atomic_ref<std::uint32_t> ver(const_cast<std::uint32_t&>(version_[h]));
    for (;;) {
        auto v1 = ver.load(std::memory_order_acquire);
        if (v1 & 1u) {
            continue;  // writer active
        }
        Snapshot s;
        s.reported = load_relaxed(reported_[h]);
        s.desired = load_relaxed(desired_[h]);
        s.confirmed_at_ns = load_relaxed(confirmed_at_ns_[h]);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (ver.load(std::memory_order_relaxed) == v1) {
            return s;
        }
    }
}

SubscriptionId Instance::subscribe(Subscription sub, std::uint64_t now_ns)
{
    SubSlot slot;
    slot.sub = std::move(sub);
    slot.accum = Bitset(model_->size());
    slot.last_refresh_ns = now_ns;
    slot.alive = true;
    // Freed slots are not reused, so a stale id can never address a newer
    // subscriber. Fine at fan-out scale (tens of subscribers).
    subs_.push_back(std::move(slot));
    return SubscriptionId(subs_.size() - 1);
}

void Instance::unsubscribe(SubscriptionId id)
{
    if (id < subs_.size()) {
        subs_[id].alive = false;
        subs_[id].accum = Bitset();
    }
}

void Instance::distribute_dirty()
{
    if (dirty_.none()) {
        return;
    }
    for (auto& slot : subs_) {
        if (slot.alive) {
            slot.accum.or_and(dirty_, slot.sub.interest);
        }
    }
    dirty_.clear();
}

}  // namespace statusbar::controlplane
