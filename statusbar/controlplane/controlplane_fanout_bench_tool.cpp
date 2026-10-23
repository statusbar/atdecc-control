// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Change fan-out benchmark: ControlPlane-style (virtual comparator + std::set)
// vs dense-handle bitset style, and — since M1 — the real controlplane
// Instance/tick() implementation, so the design-justifying numbers
// (docs/PLAN.md §4.4) cannot silently rot.
//
// With --check the tool exits non-zero if the real implementation's fan-out
// drain is more than 10x the raw-bitset reference measured in the same
// binary, or its mark cost leaves the O(1)-nanoseconds class. Enforcement is
// limited to optimized builds; Debug builds still run everything as a smoke
// test.

#include "statusbar/controlplane/controlplane.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;
namespace cp = statusbar::controlplane;

static double ms(Clock::time_point a, Clock::time_point b)
{
    return chrono::duration<double, milli>(b - a).count();
}

// ---- ControlPlane-shaped identity ----
struct ControlIdentity
{
    uint16_t type, index, section, item, h, w;
    bool operator<(ControlIdentity const& o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};

struct Comparator
{
    virtual ~Comparator() {}
    virtual bool containsControl(ControlIdentity const&) const = 0;
};
struct ComparatorSet : Comparator
{
    set<ControlIdentity> items;
    bool containsControl(ControlIdentity const& i) const override { return items.find(i) != items.end(); }
};
struct State
{
    set<ControlIdentity> changed;
};

int main(int argc, char** argv)
{
    bool check = argc > 1 && string(argv[1]) == "--check";

    int const NCP = 10624;  // control points
    int const NSUB = 64;    // subscribers
    int const NINT = 256;   // control points each subscriber cares about
    int const NEV = 20000;  // change events

    mt19937 rng(12345);

    // identities
    vector<ControlIdentity> ids(NCP);
    for (int i = 0; i < NCP; ++i) {
        ids[i] = {0x001a, (uint16_t)i, 0, 0, 0, 0};
    }

    // event stream
    vector<int> events(NEV);
    for (auto& e : events) {
        e = rng() % NCP;
    }

    // interest sets
    vector<vector<int>> interest(NSUB);
    for (int s = 0; s < NSUB; ++s) {
        int base = (s * NINT) % NCP;
        for (int k = 0; k < NINT; ++k) {
            interest[s].push_back((base + k) % NCP);
        }
    }

    // ---------- A: comparator + std::set ----------
    {
        vector<shared_ptr<ComparatorSet>> subs;
        map<shared_ptr<Comparator>, State> table;
        for (int s = 0; s < NSUB; ++s) {
            auto c = make_shared<ComparatorSet>();
            for (int k : interest[s]) {
                c->items.insert(ids[k]);
            }
            subs.push_back(c);
            table[c] = State{};
        }
        auto t0 = Clock::now();
        size_t marked = 0;
        for (int e : events) {
            ControlIdentity const& id = ids[e];
            for (auto& kv : table) {
                if (kv.first->containsControl(id)) {
                    kv.second.changed.insert(id);
                    ++marked;
                }
            }
        }
        auto t1 = Clock::now();
        // one tick: drain
        size_t drained = 0;
        for (auto& kv : table) {
            drained += kv.second.changed.size();
            kv.second.changed.clear();
        }
        auto t2 = Clock::now();
        printf("A  comparator+set  mark   %8.0f ns/change   (%zu marks)\n", ms(t0, t1) * 1e6 / NEV, marked);
        printf("   comparator+set  drain  %8.1f us/tick     (%zu items)\n", ms(t1, t2) * 1000.0, drained);
    }

    // ---------- B: dense handles + bitsets (raw reference) ----------
    double b_mark_ns = 0, b_drain_us = 0;
    {
        int const W = (NCP + 63) / 64;
        vector<uint64_t> dirty(W, 0);
        vector<vector<uint64_t>> subint(NSUB, vector<uint64_t>(W, 0));
        for (int s = 0; s < NSUB; ++s) {
            for (int k : interest[s]) {
                subint[s][k >> 6] |= 1ull << (k & 63);
            }
        }

        auto t0 = Clock::now();
        for (int e : events) {
            dirty[e >> 6] |= 1ull << (e & 63);
        }
        auto t1 = Clock::now();

        // one tick: AND per subscriber, emit handles
        vector<uint32_t> out;
        out.reserve(NINT);
        size_t delivered = 0;
        auto t2 = Clock::now();
        for (int s = 0; s < NSUB; ++s) {
            out.clear();
            for (int w = 0; w < W; ++w) {
                uint64_t m = dirty[w] & subint[s][w];
                while (m) {
                    int b = __builtin_ctzll(m);
                    m &= m - 1;
                    out.push_back((uint32_t)((w << 6) + b));
                }
            }
            delivered += out.size();
        }
        fill(dirty.begin(), dirty.end(), 0ull);
        auto t3 = Clock::now();
        b_mark_ns = ms(t0, t1) * 1e6 / NEV;
        b_drain_us = ms(t2, t3) * 1000.0;
        printf("B  bitset          mark   %8.1f ns/change\n", b_mark_ns);
        printf("   bitset          drain  %8.1f us/tick     (%zu items)\n", b_drain_us, delivered);
    }

    // ---------- C: the real controlplane Instance + tick() ----------
    double c_mark_ns = 0, c_drain_us = 0;
    {
        cp::ModelBuilder builder;
        for (int i = 0; i < NCP; ++i) {
            builder.add(cp::ControlPointDesc{});
        }
        cp::Instance inst(builder.build(), cp::Role::entity);

        uint64_t const T0 = 1'000'000'000ull;
        for (int s = 0; s < NSUB; ++s) {
            cp::Subscription sub;
            sub.interest = cp::Selector(NCP);
            for (int k : interest[s]) {
                sub.interest.set((cp::ControlPointId)k);
            }
            sub.throttle_ms = 0;
            sub.refresh_ms = 3'600'000;  // out of the picture
            inst.subscribe(std::move(sub), T0);
        }

        auto t0 = Clock::now();
        uint64_t v = 0;
        for (int e : events) {
            inst.set_local((cp::ControlPointId)e, ++v, T0);
        }
        auto t1 = Clock::now();

        size_t delivered = 0;
        auto t2 = Clock::now();
        inst.tick(T0 + 1'000'000ull, [&](cp::SubscriptionId, cp::Selector const& changed) {
            changed.for_each_set([&](cp::ControlPointId) { ++delivered; });
        });
        auto t3 = Clock::now();
        c_mark_ns = ms(t0, t1) * 1e6 / NEV;
        c_drain_us = ms(t2, t3) * 1000.0;
        printf("C  Instance        mark   %8.1f ns/change\n", c_mark_ns);
        printf("   Instance/tick   drain  %8.1f us/tick     (%zu items)\n", c_drain_us, delivered);
    }

    if (check) {
#ifdef NDEBUG
        bool ok = true;
        if (c_drain_us > 10.0 * b_drain_us) {
            printf("CHECK FAIL: Instance drain %.1f us > 10x raw bitset %.1f us\n", c_drain_us, b_drain_us);
            ok = false;
        }
        if (c_mark_ns > 100.0) {
            printf("CHECK FAIL: Instance mark %.1f ns/change left the O(1)-ns class\n", c_mark_ns);
            ok = false;
        }
        if (!ok) {
            return 1;
        }
        printf("CHECK OK\n");
#else
        printf("CHECK SKIPPED (unoptimized build)\n");
#endif
    }
    return 0;
}
