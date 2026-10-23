// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Pattern-selection benchmark over a synthetic control-point address space,
// comparing regex / linear scan / raw trie / SQLite — and, since M2, the real
// PathAdaptor implementation (docs/PLAN.md §4.7).
//
// With --check the tool exits non-zero unless the real PathAdaptor selects
// the 256-of-10k pattern in under 10 us — the M2 acceptance criterion.
// Enforcement is limited to optimized builds; the hit-count correctness
// check applies always.
#include "sqlite3.h"
#include "statusbar/controlplane/controlplane.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;

static double ms(Clock::time_point a, Clock::time_point b)
{
    return chrono::duration<double, milli>(b - a).count();
}

// ---------- address space ----------
static vector<string> g_paths;

static void build_paths()
{
    char buf[128];
    for (int i = 0; i < 256; ++i) {
        for (char const* p : {"gain", "mute", "polarity", "delay"}) {
            snprintf(buf, sizeof buf, "/input/%d/%s", i, p);
            g_paths.push_back(buf);
        }
        for (int b = 0; b < 8; ++b) {
            for (char const* p : {"freq", "gain", "q"}) {
                snprintf(buf, sizeof buf, "/input/%d/eq/%d/%s", i, b, p);
                g_paths.push_back(buf);
            }
        }
    }
    for (int o = 0; o < 128; ++o) {
        for (char const* p : {"gain", "mute", "delay"}) {
            snprintf(buf, sizeof buf, "/output/%d/%s", o, p);
            g_paths.push_back(buf);
        }
        for (int b = 0; b < 8; ++b) {
            for (char const* p : {"freq", "gain", "q"}) {
                snprintf(buf, sizeof buf, "/output/%d/eq/%d/%s", o, b, p);
                g_paths.push_back(buf);
            }
        }
    }
}

// ---------- segment interning ----------
static uint64_t const NUMTAG = 1ull << 63;
static unordered_map<string, uint64_t> g_symtab;
static uint64_t intern(string const& s)
{
    bool num = !s.empty();
    for (char c : s) {
        if (c < '0' || c > '9') {
            num = false;
            break;
        }
    }
    if (num) {
        return NUMTAG | (uint64_t)stoull(s);
    }
    auto it = g_symtab.find(s);
    if (it != g_symtab.end()) {
        return it->second;
    }
    uint64_t id = g_symtab.size() + 1;
    g_symtab[s] = id;
    return id;
}
static vector<string> split(string const& p)
{
    vector<string> r;
    size_t i = 0;
    while (i < p.size()) {
        if (p[i] == '/') {
            ++i;
            continue;
        }
        size_t j = p.find('/', i);
        if (j == string::npos) {
            j = p.size();
        }
        r.push_back(p.substr(i, j - i));
        i = j;
    }
    return r;
}

static vector<vector<uint64_t>> g_segs;  // interned, per control point

// ---------- selector ----------
struct Seg
{
    enum K
    {
        LIT,
        ANYINT,
        ANY
    } k;
    uint64_t key;
};
static vector<Seg> make_selector(vector<string> const& pat)
{
    vector<Seg> s;
    for (auto& p : pat) {
        if (p == "\\d+" || p == "#") {
            s.push_back({Seg::ANYINT, 0});
        } else if (p == "*") {
            s.push_back({Seg::ANY, 0});
        } else {
            s.push_back({Seg::LIT, intern(p)});
        }
    }
    return s;
}
static bool seg_match(Seg const& s, uint64_t v)
{
    switch (s.k) {
        case Seg::LIT:
            return v == s.key;
        case Seg::ANYINT:
            return (v & NUMTAG) != 0;
        default:
            return true;
    }
}

// ---------- trie ----------
struct Node
{
    unordered_map<uint64_t, uint32_t> kids;
    int32_t leaf = -1;
};
static vector<Node> g_trie;

static void trie_build()
{
    g_trie.clear();
    g_trie.emplace_back();
    for (size_t id = 0; id < g_segs.size(); ++id) {
        uint32_t n = 0;
        for (uint64_t s : g_segs[id]) {
            auto it = g_trie[n].kids.find(s);
            if (it == g_trie[n].kids.end()) {
                uint32_t nn = g_trie.size();
                g_trie.emplace_back();
                g_trie[n].kids[s] = nn;
                n = nn;
            } else {
                n = it->second;
            }
        }
        g_trie[n].leaf = (int32_t)id;
    }
}
static void trie_walk(uint32_t n, vector<Seg> const& sel, size_t d, vector<int32_t>& out)
{
    if (d == sel.size()) {
        if (g_trie[n].leaf >= 0) {
            out.push_back(g_trie[n].leaf);
        }
        return;
    }
    Seg const& s = sel[d];
    if (s.k == Seg::LIT) {
        auto it = g_trie[n].kids.find(s.key);
        if (it != g_trie[n].kids.end()) {
            trie_walk(it->second, sel, d + 1, out);
        }
    } else {
        for (auto& kv : g_trie[n].kids) {
            if (seg_match(s, kv.first)) {
                trie_walk(kv.second, sel, d + 1, out);
            }
        }
    }
}

namespace cp = statusbar::controlplane;

// ---------- main ----------
int main(int argc, char** argv)
{
    bool check = argc > 1 && string(argv[1]) == "--check";
    build_paths();
    size_t const N = g_paths.size();
    g_segs.reserve(N);
    for (auto& p : g_paths) {
        vector<uint64_t> v;
        for (auto& s : split(p)) {
            v.push_back(intern(s));
        }
        g_segs.push_back(std::move(v));
    }
    trie_build();
    printf("control points: %zu\n", N);

    // address map as in ControlPlane: std::map<string, id>
    map<string, uint32_t> addr_map;
    for (size_t i = 0; i < N; ++i) {
        addr_map[g_paths[i]] = (uint32_t)i;
    }

    // sqlite: in-memory, columns per segment + full path
    sqlite3* db = nullptr;
    sqlite3_open(":memory:", &db);
    sqlite3_exec(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;", 0, 0, 0);
    sqlite3_exec(
        db,
        "CREATE TABLE cp(id INTEGER PRIMARY KEY, path TEXT, depth INT,"
        " s0 TEXT, s1 INT, s2 TEXT, s3 INT, s4 TEXT);",
        0,
        0,
        0);
    sqlite3_exec(db, "BEGIN", 0, 0, 0);
    {
        sqlite3_stmt* ins;
        sqlite3_prepare_v2(db, "INSERT INTO cp VALUES(?,?,?,?,?,?,?,?)", -1, &ins, 0);
        for (size_t i = 0; i < N; ++i) {
            auto sv = split(g_paths[i]);
            sqlite3_reset(ins);
            sqlite3_bind_int64(ins, 1, (sqlite3_int64)i);
            sqlite3_bind_text(ins, 2, g_paths[i].c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(ins, 3, (int)sv.size());
            for (int c = 0; c < 5; ++c) {
                if (c < (int)sv.size()) {
                    if (c == 1 || c == 3) {
                        sqlite3_bind_int(ins, 4 + c, atoi(sv[c].c_str()));
                    } else {
                        sqlite3_bind_text(ins, 4 + c, sv[c].c_str(), -1, SQLITE_TRANSIENT);
                    }
                } else {
                    sqlite3_bind_null(ins, 4 + c);
                }
            }
            sqlite3_step(ins);
        }
        sqlite3_finalize(ins);
    }
    sqlite3_exec(db, "COMMIT", 0, 0, 0);
    sqlite3_exec(db, "CREATE INDEX ix ON cp(s0, s2, depth);", 0, 0, 0);
    sqlite3_exec(db, "ANALYZE", 0, 0, 0);

    int const REPS = 200;
    string const rx_pat = "/input/\\d+/gain";
    string const glob_pat = "/input/*/gain";
    auto sel = make_selector({"input", "\\d+", "gain"});

    size_t expect = 0;
    for (auto& p : g_paths) {
        if (p.rfind("/input/", 0) == 0 && p.size() > 5 && p.compare(p.size() - 5, 5, "/gain") == 0 &&
            count(p.begin(), p.end(), '/') == 3) {
            ++expect;
        }
    }
    printf("expected matches: %zu\n\n", expect);

    // A. std::regex construction cost
    {
        auto t0 = Clock::now();
        for (int r = 0; r < REPS; ++r) {
            regex re(rx_pat);
            (void)re;
        }
        auto t1 = Clock::now();
        printf("A0 std::regex construction        %8.1f us/op\n", ms(t0, t1) * 1000.0 / REPS);
    }
    // A. std::regex full scan (ControlPlane's getAddressesForRegex)
    {
        regex re(rx_pat);
        size_t hits = 0;
        auto t0 = Clock::now();
        for (int r = 0; r < REPS; ++r) {
            set<uint32_t> out;
            for (auto& kv : addr_map) {
                if (regex_match(kv.first, re)) {
                    out.insert(kv.second);
                }
            }
            hits = out.size();
        }
        auto t1 = Clock::now();
        printf("A1 std::regex scan over map       %8.1f us/op  (%zu hits)\n", ms(t0, t1) * 1000.0 / REPS, hits);
    }
    // B. precompiled selector, linear scan over interned segments
    {
        size_t hits = 0;
        auto t0 = Clock::now();
        for (int r = 0; r < REPS; ++r) {
            vector<int32_t> out;
            for (size_t i = 0; i < N; ++i) {
                auto const& sv = g_segs[i];
                if (sv.size() != sel.size()) {
                    continue;
                }
                bool ok = true;
                for (size_t d = 0; d < sel.size(); ++d) {
                    if (!seg_match(sel[d], sv[d])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    out.push_back((int32_t)i);
                }
            }
            hits = out.size();
        }
        auto t1 = Clock::now();
        printf("B  selector linear scan           %8.1f us/op  (%zu hits)\n", ms(t0, t1) * 1000.0 / REPS, hits);
    }
    // C. trie walk
    {
        size_t hits = 0;
        auto t0 = Clock::now();
        for (int r = 0; r < REPS; ++r) {
            vector<int32_t> out;
            trie_walk(0, sel, 0, out);
            hits = out.size();
        }
        auto t1 = Clock::now();
        printf("C  trie walk                      %8.1f us/op  (%zu hits)\n", ms(t0, t1) * 1000.0 / REPS, hits);
    }
    // D1. sqlite indexed prepared statement
    {
        sqlite3_stmt* st;
        sqlite3_prepare_v2(db, "SELECT id FROM cp WHERE s0=? AND s2=? AND depth=3", -1, &st, 0);
        size_t hits = 0;
        auto t0 = Clock::now();
        for (int r = 0; r < REPS; ++r) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, "input", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, "gain", -1, SQLITE_STATIC);
            vector<int32_t> out;
            while (sqlite3_step(st) == SQLITE_ROW) {
                out.push_back(sqlite3_column_int(st, 0));
            }
            hits = out.size();
        }
        auto t1 = Clock::now();
        sqlite3_finalize(st);
        printf("D1 sqlite indexed prepared stmt   %8.1f us/op  (%zu hits)\n", ms(t0, t1) * 1000.0 / REPS, hits);
    }
    // D2. sqlite GLOB full scan
    {
        sqlite3_stmt* st;
        sqlite3_prepare_v2(db, "SELECT id FROM cp WHERE path GLOB ?", -1, &st, 0);
        size_t hits = 0;
        auto t0 = Clock::now();
        for (int r = 0; r < REPS; ++r) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, glob_pat.c_str(), -1, SQLITE_STATIC);
            vector<int32_t> out;
            while (sqlite3_step(st) == SQLITE_ROW) {
                out.push_back(sqlite3_column_int(st, 0));
            }
            hits = out.size();
        }
        auto t1 = Clock::now();
        sqlite3_finalize(st);
        printf("D2 sqlite GLOB full scan          %8.1f us/op  (%zu hits)\n", ms(t0, t1) * 1000.0 / REPS, hits);
    }
    // E. single point lookup for reference
    {
        sqlite3_stmt* st;
        sqlite3_prepare_v2(db, "SELECT id FROM cp WHERE path=?", -1, &st, 0);
        sqlite3_exec(db, "CREATE INDEX ixp ON cp(path)", 0, 0, 0);
        string const one = "/input/137/gain";
        uint32_t volatile sink = 0;
        auto t0 = Clock::now();
        for (int r = 0; r < REPS * 50; ++r) {
            auto it = addr_map.find(one);
            sink += it->second;
        }
        auto t1 = Clock::now();
        for (int r = 0; r < REPS * 50; ++r) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, one.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) {
                sink += sqlite3_column_int(st, 0);
            }
        }
        auto t2 = Clock::now();
        sqlite3_finalize(st);
        printf("\nE1 std::map<string> point lookup  %8.0f ns/op\n", ms(t0, t1) * 1e6 / (REPS * 50));
        printf("E2 sqlite indexed point lookup    %8.0f ns/op\n", ms(t1, t2) * 1e6 / (REPS * 50));
    }
    sqlite3_close(db);

    // F. the real PathAdaptor: trie walk to a Selector, and point lookup
    double f_pattern_us = 0;
    size_t f_hits = 0;
    {
        cp::PathAdaptorBuilder builder(N);
        for (size_t i = 0; i < N; ++i) {
            builder.map(g_paths[i], (cp::ControlPointId)i);
        }
        auto adaptor = builder.build();

        auto t0 = Clock::now();
        for (int r = 0; r < REPS; ++r) {
            auto s = adaptor.resolve_pattern("/input/#/gain");
            f_hits = s.count();
        }
        auto t1 = Clock::now();
        f_pattern_us = ms(t0, t1) * 1000.0 / REPS;

        uint32_t volatile sink = 0;
        auto t2 = Clock::now();
        for (int r = 0; r < REPS * 50; ++r) {
            sink += *adaptor.resolve("/input/137/gain");
        }
        auto t3 = Clock::now();
        printf("\nF  PathAdaptor resolve_pattern    %8.1f us/op  (%zu hits)\n", f_pattern_us, f_hits);
        printf("   PathAdaptor point resolve      %8.0f ns/op\n", ms(t2, t3) * 1e6 / (REPS * 50));
    }

    if (f_hits != expect) {
        printf("CHECK FAIL: PathAdaptor found %zu hits, expected %zu\n", f_hits, expect);
        return 1;
    }
    if (check) {
#ifdef NDEBUG
        if (f_pattern_us > 10.0) {
            printf("CHECK FAIL: resolve_pattern %.1f us/op exceeds the 10 us acceptance bound\n", f_pattern_us);
            return 1;
        }
        printf("CHECK OK\n");
#else
        printf("CHECK SKIPPED (unoptimized build)\n");
#endif
    }
    return 0;
}
