// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

// Dense bitset over the ControlPointId handle space (docs/PLAN.md §4.3).
// One type serves subscription interest, write permission, and scene
// contents; Unique/None/All are degenerate cases and composition is bitwise.
// Invariant: bits past size() in the last word are always zero, so word-wise
// equality and count() need no special casing.

namespace statusbar::controlplane {

using ControlPointId = std::uint32_t;

class Bitset
{
  public:
    Bitset() = default;
    explicit Bitset(std::size_t bit_count) { resize(bit_count); }

    std::size_t size() const noexcept { return bit_count_; }

    /// Grow or shrink to bit_count bits; existing bits are preserved, new
    /// bits are zero.
    void resize(std::size_t bit_count)
    {
        bit_count_ = bit_count;
        words_.resize((bit_count + WORD_BITS - 1) / WORD_BITS, 0);
        mask_tail();
    }

    bool test(ControlPointId h) const
    {
        assert(h < bit_count_);
        return (words_[h / WORD_BITS] >> (h % WORD_BITS)) & 1u;
    }

    void set(ControlPointId h)
    {
        assert(h < bit_count_);
        words_[h / WORD_BITS] |= std::uint64_t(1) << (h % WORD_BITS);
    }

    void reset(ControlPointId h)
    {
        assert(h < bit_count_);
        words_[h / WORD_BITS] &= ~(std::uint64_t(1) << (h % WORD_BITS));
    }

    /// Zero every bit.
    void clear() noexcept
    {
        for (auto& w : words_) {
            w = 0;
        }
    }

    /// Set every bit (the degenerate "All" selector).
    void set_all()
    {
        for (auto& w : words_) {
            w = ~std::uint64_t(0);
        }
        mask_tail();
    }

    bool any() const noexcept
    {
        for (auto w : words_) {
            if (w) {
                return true;
            }
        }
        return false;
    }

    bool none() const noexcept { return !any(); }

    std::size_t count() const noexcept
    {
        std::size_t n = 0;
        for (auto w : words_) {
            n += std::size_t(std::popcount(w));
        }
        return n;
    }

    /// Both operands must be the same size.
    Bitset& operator|=(Bitset const& o)
    {
        assert(bit_count_ == o.bit_count_);
        for (std::size_t i = 0; i < words_.size(); ++i) {
            words_[i] |= o.words_[i];
        }
        return *this;
    }

    /// Both operands must be the same size.
    Bitset& operator&=(Bitset const& o)
    {
        assert(bit_count_ == o.bit_count_);
        for (std::size_t i = 0; i < words_.size(); ++i) {
            words_[i] &= o.words_[i];
        }
        return *this;
    }

    /// this |= (a & b), without materialising a temporary. All three must be
    /// the same size. This is the per-subscriber accumulation step of the
    /// change fan-out: accum |= dirty & interest.
    void or_and(Bitset const& a, Bitset const& b)
    {
        assert(bit_count_ == a.bit_count_ && bit_count_ == b.bit_count_);
        for (std::size_t i = 0; i < words_.size(); ++i) {
            words_[i] |= a.words_[i] & b.words_[i];
        }
    }

    friend bool operator==(Bitset const&, Bitset const&) = default;

    /// Visit every set bit in ascending handle order.
    template <typename Visit>
    void for_each_set(Visit&& visit) const
    {
        for (std::size_t i = 0; i < words_.size(); ++i) {
            std::uint64_t w = words_[i];
            while (w) {
                auto bit = unsigned(std::countr_zero(w));
                visit(ControlPointId(i * WORD_BITS + bit));
                w &= w - 1;
            }
        }
    }

  private:
    static constexpr std::size_t WORD_BITS = 64;

    void mask_tail()
    {
        auto rem = bit_count_ % WORD_BITS;
        if (rem != 0 && !words_.empty()) {
            words_.back() &= (std::uint64_t(1) << rem) - 1;
        }
    }

    std::vector<std::uint64_t> words_;
    std::size_t bit_count_ = 0;
};

// Selectors are bitsets (docs/PLAN.md §4.3): pattern compilation (M2)
// produces one, and subscription interest, write permission and scene masks
// consume it.
using Selector = Bitset;

}  // namespace statusbar::controlplane
