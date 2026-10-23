// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane/controlplane_adaptor.hpp"

#include <cassert>

namespace statusbar::controlplane {

namespace {

/// Encodes one address segment for lookup (never interning); nullopt when
/// the segment is a symbol this table has never seen.
std::optional<SegmentKey> lookup_key(SymbolTable const& symbols, std::string_view seg)
{
    if (auto num = parse_numeric_segment(seg)) {
        return numeric_key(*num);
    }
    auto id = symbols.find(seg);
    if (id == SymbolTable::INVALID) {
        return std::nullopt;
    }
    return symbol_key(id);
}

}  // namespace

std::optional<ControlPointId> PathAdaptor::resolve(std::string_view path) const
{
    std::vector<SegmentKey> segments;
    bool unknown = false;
    split_path(path, [&](std::string_view seg) {
        if (auto key = lookup_key(symbols_, seg)) {
            segments.push_back(*key);
        } else {
            unknown = true;
        }
    });
    if (unknown || segments.empty()) {
        return std::nullopt;
    }
    return trie_.find(segments);
}

Selector PathAdaptor::resolve_pattern(std::string_view pattern) const
{
    Selector out(paths_.size());
    std::vector<PatternSeg> compiled;
    bool unknown = false;
    split_path(pattern, [&](std::string_view seg) {
        if (seg == "*") {
            compiled.push_back({PatternSeg::Kind::any, 0});
        } else if (seg == "#") {
            compiled.push_back({PatternSeg::Kind::any_number, 0});
        } else if (auto key = lookup_key(symbols_, seg)) {
            compiled.push_back({PatternSeg::Kind::literal, *key});
        } else {
            unknown = true;
        }
    });
    if (unknown || compiled.empty()) {
        return out;  // an unseen symbol can match nothing
    }
    trie_.match(std::span<PatternSeg const>(compiled), [&](ControlPointId h) { out.set(h); });
    return out;
}

bool PathAdaptorBuilder::map(std::string_view path, ControlPointId h)
{
    if (h >= adaptor_.paths_.size() || !adaptor_.paths_[h].empty()) {
        return false;
    }
    std::vector<SegmentKey> segments;
    split_path(path, [&](std::string_view seg) {
        if (auto num = parse_numeric_segment(seg)) {
            segments.push_back(numeric_key(*num));
        } else {
            segments.push_back(symbol_key(adaptor_.symbols_.intern(seg)));
        }
    });
    if (segments.empty()) {
        return false;
    }
    if (!trie_.insert(segments, h)) {
        return false;
    }
    adaptor_.paths_[h].assign(path);
    return true;
}

PathAdaptor PathAdaptorBuilder::build()
{
    adaptor_.trie_image_.resize(trie_.image_size());
    auto const written = trie_.write_image(adaptor_.trie_image_);
    assert(written == adaptor_.trie_image_.size());
    (void)written;
    adaptor_.attach();
    assert(adaptor_.trie_.valid());

    auto handle_space = adaptor_.paths_.size();
    PathAdaptor out = std::move(adaptor_);
    adaptor_ = PathAdaptor{};
    adaptor_.paths_.resize(handle_space);
    trie_ = SegmentTrie{};
    return out;
}

}  // namespace statusbar::controlplane
