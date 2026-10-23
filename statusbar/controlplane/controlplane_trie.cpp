// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane/controlplane_trie.hpp"

#include <algorithm>
#include <charconv>

namespace statusbar::controlplane {

std::optional<std::uint64_t> parse_numeric_segment(std::string_view seg) noexcept
{
    if (seg.empty() || seg.size() > 19) {
        return std::nullopt;
    }
    if (seg.size() > 1 && seg.front() == '0') {
        return std::nullopt;  // leading zero: keep it a symbol
    }
    std::uint64_t value = 0;
    auto [ptr, ec] = std::from_chars(seg.data(), seg.data() + seg.size(), value);
    if (ec != std::errc{} || ptr != seg.data() + seg.size()) {
        return std::nullopt;
    }
    if ((value & SEGMENT_NUMERIC_TAG) != 0) {
        return std::nullopt;  // would collide with the tag bit
    }
    return value;
}

SegmentTrie::SegmentTrie()
{
    grow(64, 64);
}

void SegmentTrie::grow(std::size_t min_nodes, std::size_t min_edges)
{
    using Builder = container::FlatTrieBuilder<SegmentKey, ControlPointId>;
    auto const nodes = std::max(min_nodes, trie_.max_nodes() * 2);
    auto const edges = std::max(min_edges, trie_.max_edges() * 2);
    std::vector<std::byte> bigger(Builder::storage_size(nodes, edges));
    // operator new aligns to max_align_t (16) >= storage_align.
    trie_ = trie_.max_nodes() == 0 ? Builder(bigger, nodes, edges) : Builder(bigger, nodes, edges, trie_);
    storage_ = std::move(bigger);
}

bool SegmentTrie::insert(std::span<SegmentKey const> segments, ControlPointId leaf)
{
    using Insert = container::FlatTrieBuilder<SegmentKey, ControlPointId>::Insert;
    for (;;) {
        switch (trie_.insert(segments, leaf)) {
            case Insert::ok:
                return true;
            case Insert::exists:
                return false;
            case Insert::full:
                grow(trie_.node_count() + segments.size(), trie_.edge_count() + segments.size());
                break;
        }
    }
}

ControlPointId SegmentTrie::find(std::span<SegmentKey const> segments) const
{
    auto leaf = trie_.find(segments);
    return leaf ? *leaf : NO_CONTROL_POINT;
}

}  // namespace statusbar::controlplane
