// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/container/container_flat_trie.hpp"
#include "statusbar/controlplane/controlplane_bitset.hpp"
#include "statusbar/controlplane/controlplane_symbols.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Segment-level trie over interned symbols (docs/PLAN.md §4.7). One node
// hop per path segment; numeric segments are tagged integers, not digit
// chains, so /input/137/gain is three comparisons, not fifteen.
//
// The container itself is core's FlatTrie (statusbar/container): zero-heap
// over caller storage, with a pointer-free image format ready for a
// read-only mapped buffer (the model store milestone). This layer owns the
// SegmentKey encoding, the slash-separated path conventions, and the `*`/`#` pattern
// grammar; SegmentTrie is the growable build phase (it owns heap storage
// for the core builder and doubles it on demand), and PathAdaptor queries
// the frozen image through a FlatTrieView.
//
// std::regex is banned on this path by measurement: see the selection bench.

namespace statusbar::controlplane {

constexpr ControlPointId NO_CONTROL_POINT = 0xffffffffu;

/// One path segment, encoded for trie comparison: either an interned symbol
/// id or a numeric value with the top bit set.
using SegmentKey = std::uint64_t;

constexpr SegmentKey SEGMENT_NUMERIC_TAG = SegmentKey(1) << 63;

constexpr SegmentKey symbol_key(SymbolTable::SymbolId id) noexcept
{
    return SegmentKey(id);
}

constexpr SegmentKey numeric_key(std::uint64_t value) noexcept
{
    return SEGMENT_NUMERIC_TAG | value;
}

constexpr bool is_numeric_key(SegmentKey k) noexcept
{
    return (k & SEGMENT_NUMERIC_TAG) != 0;
}

/// A segment is numeric only when it is a canonical decimal integer — no
/// leading zeros (except "0" itself), digits only, and small enough not to
/// collide with the tag bit. "007" stays a symbol so it cannot alias "7".
std::optional<std::uint64_t> parse_numeric_segment(std::string_view seg) noexcept;

/// Splits an address path on '/' (empty segments from leading, trailing or
/// doubled slashes are dropped) and visits each segment in order.
template <typename Visit>
void split_path(std::string_view path, Visit&& visit)
{
    std::size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/') {
            ++i;
            continue;
        }
        auto j = path.find('/', i);
        if (j == std::string_view::npos) {
            j = path.size();
        }
        visit(path.substr(i, j - i));
        i = j;
    }
}

/// One compiled pattern segment: a literal key, "any segment" (*), or "any
/// numeric segment" (#). Satisfies container::TrieMatcher.
struct PatternSeg
{
    enum class Kind : std::uint8_t
    {
        literal,
        any,
        any_number,
    };

    Kind kind = Kind::literal;
    SegmentKey key = 0;

    bool is_literal() const noexcept { return kind == Kind::literal; }
    SegmentKey literal_key() const noexcept { return key; }
    bool matches(SegmentKey k) const noexcept { return kind == Kind::any || is_numeric_key(k); }
};

/// The growable build phase: a core FlatTrieBuilder plus the heap storage
/// it carves from, doubled when an insert reports full.
class SegmentTrie
{
  public:
    SegmentTrie();

    // The builder holds pointers into storage_: moves keep the heap block
    // (and the pointers) valid; copies would not, and nothing needs them.
    SegmentTrie(SegmentTrie const&) = delete;
    SegmentTrie& operator=(SegmentTrie const&) = delete;
    SegmentTrie(SegmentTrie&&) noexcept = default;
    SegmentTrie& operator=(SegmentTrie&&) noexcept = default;

    /// Maps a full segment path to a leaf handle. Returns false when the
    /// exact path is already mapped (first mapping wins).
    bool insert(std::span<SegmentKey const> segments, ControlPointId leaf);

    /// Exact walk; NO_CONTROL_POINT when the path names nothing.
    ControlPointId find(std::span<SegmentKey const> segments) const;

    /// Visits the leaf of every path matching the compiled pattern at
    /// exact depth.
    template <typename Visit>
    void match(std::span<PatternSeg const> pattern, Visit&& visit) const
    {
        trie_.match(pattern, [&](ControlPointId leaf) { visit(leaf); });
    }

    std::size_t node_count() const noexcept { return trie_.node_count(); }

    /// The frozen, mmap-able image (see container_flat_trie.hpp for the
    /// format): size, and serialization into a caller buffer aligned to
    /// FlatTrieView::image_align.
    std::size_t image_size() const noexcept { return trie_.image_size(); }
    std::size_t write_image(std::span<std::byte> out) const noexcept { return trie_.write_image(out); }

  private:
    void grow(std::size_t min_nodes, std::size_t min_edges);

    std::vector<std::byte> storage_;
    container::FlatTrieBuilder<SegmentKey, ControlPointId> trie_;
};

/// The query phase: a validated view over a SegmentTrie image.
using SegmentTrieView = container::FlatTrieView<SegmentKey, ControlPointId>;

}  // namespace statusbar::controlplane
