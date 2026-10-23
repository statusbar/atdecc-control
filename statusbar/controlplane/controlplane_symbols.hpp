// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Interned address-path segments (docs/PLAN.md §4.7). Address resolution
// works on interned symbol ids, never on characters, so a trie node compares
// one integer per segment. Numeric segments are not interned at all — they
// travel as tagged integers (see controlplane_trie.hpp).

namespace statusbar::controlplane {

class SymbolTable
{
  public:
    using SymbolId = std::uint32_t;
    static constexpr SymbolId INVALID = 0xffffffffu;

    /// Returns the existing id or interns a new one.
    SymbolId intern(std::string_view s);

    /// Lookup without interning; INVALID when the symbol was never seen. A
    /// miss during resolution means "cannot match anything" — resolving must
    /// never grow the table.
    SymbolId find(std::string_view s) const;

    std::string_view name(SymbolId id) const { return names_[id]; }
    std::size_t size() const noexcept { return names_.size(); }

  private:
    struct Hash
    {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };

    std::vector<std::string> names_;
    std::unordered_map<std::string, SymbolId, Hash, std::equal_to<>> index_;
};

}  // namespace statusbar::controlplane
