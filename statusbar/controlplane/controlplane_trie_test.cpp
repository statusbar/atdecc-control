// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for SymbolTable, segment parsing, and SegmentTrie.

#include "statusbar/controlplane/controlplane_trie.hpp"

#include "statusbar/test/test.hpp"

#include <string_view>
#include <vector>

using namespace statusbar;
using namespace statusbar::controlplane;

TEST(symbols, intern_dedupes_and_round_trips)
{
    SymbolTable t;
    auto a = t.intern("input");
    auto b = t.intern("gain");
    auto a2 = t.intern("input");
    EXPECT_EQ(a, a2);
    EXPECT_NE(a, b);
    EXPECT_EQ(t.size(), 2u);
    EXPECT_TRUE(t.name(a) == "input");
    EXPECT_TRUE(t.name(b) == "gain");
    EXPECT_EQ(t.find("input"), a);
    EXPECT_EQ(t.find("nope"), SymbolTable::INVALID);
    EXPECT_EQ(t.size(), 2u);  // find never interns
}

TEST(segments, numeric_parse_is_canonical)
{
    EXPECT_TRUE(parse_numeric_segment("0").has_value());
    EXPECT_EQ(*parse_numeric_segment("0"), 0u);
    EXPECT_EQ(*parse_numeric_segment("137"), 137u);
    // Leading zeros, signs, non-digits and empty stay symbols.
    EXPECT_FALSE(parse_numeric_segment("007").has_value());
    EXPECT_FALSE(parse_numeric_segment("-1").has_value());
    EXPECT_FALSE(parse_numeric_segment("1a").has_value());
    EXPECT_FALSE(parse_numeric_segment("").has_value());
    EXPECT_FALSE(parse_numeric_segment("99999999999999999999").has_value());
}

TEST(segments, split_path_drops_empty_segments)
{
    std::vector<std::string_view> segs;
    split_path("/input/1//gain/", [&](std::string_view s) { segs.push_back(s); });
    EXPECT_EQ(segs.size(), 3u);
    EXPECT_TRUE(segs[0] == "input");
    EXPECT_TRUE(segs[1] == "1");
    EXPECT_TRUE(segs[2] == "gain");
}

TEST(trie, insert_find_and_first_mapping_wins)
{
    SymbolTable syms;
    SegmentTrie trie;
    auto input = symbol_key(syms.intern("input"));
    auto gain = symbol_key(syms.intern("gain"));
    auto mute = symbol_key(syms.intern("mute"));

    std::vector<SegmentKey> p0{input, numeric_key(1), gain};
    std::vector<SegmentKey> p1{input, numeric_key(1), mute};
    std::vector<SegmentKey> p2{input, numeric_key(2), gain};

    EXPECT_TRUE(trie.insert(p0, 10));
    EXPECT_TRUE(trie.insert(p1, 11));
    EXPECT_TRUE(trie.insert(p2, 12));
    EXPECT_FALSE(trie.insert(p0, 99));  // duplicate path refused

    EXPECT_EQ(trie.find(p0), 10u);
    EXPECT_EQ(trie.find(p1), 11u);
    EXPECT_EQ(trie.find(p2), 12u);

    // A prefix of a mapped path is not itself mapped.
    std::vector<SegmentKey> prefix{input, numeric_key(1)};
    EXPECT_EQ(trie.find(prefix), NO_CONTROL_POINT);
    // An unknown branch resolves to nothing.
    std::vector<SegmentKey> other{input, numeric_key(3), gain};
    EXPECT_EQ(trie.find(other), NO_CONTROL_POINT);
}

TEST(trie, match_wildcards)
{
    SymbolTable syms;
    SegmentTrie trie;
    auto input = symbol_key(syms.intern("input"));
    auto master = symbol_key(syms.intern("master"));
    auto gain = symbol_key(syms.intern("gain"));

    std::vector<SegmentKey> p0{input, numeric_key(1), gain};
    std::vector<SegmentKey> p1{input, numeric_key(2), gain};
    std::vector<SegmentKey> p2{input, master, gain};
    trie.insert(p0, 0);
    trie.insert(p1, 1);
    trie.insert(p2, 2);

    auto run = [&](std::vector<PatternSeg> const& pat) {
        std::vector<ControlPointId> hits;
        trie.match(pat, [&](ControlPointId h) { hits.push_back(h); });
        return hits;
    };

    // `#` matches numeric children only.
    auto hits = run({{PatternSeg::Kind::literal, input}, {PatternSeg::Kind::any_number, 0}, {PatternSeg::Kind::literal, gain}});
    EXPECT_EQ(hits.size(), 2u);

    // `*` matches every child.
    hits = run({{PatternSeg::Kind::literal, input}, {PatternSeg::Kind::any, 0}, {PatternSeg::Kind::literal, gain}});
    EXPECT_EQ(hits.size(), 3u);

    // Matching is exact-depth: a two-segment pattern hits no three-segment path.
    hits = run({{PatternSeg::Kind::literal, input}, {PatternSeg::Kind::any, 0}});
    EXPECT_EQ(hits.size(), 0u);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane, controlplane_trie_test)
