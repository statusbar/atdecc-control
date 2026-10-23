// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane/controlplane_symbols.hpp"

namespace statusbar::controlplane {

SymbolTable::SymbolId SymbolTable::intern(std::string_view s)
{
    auto it = index_.find(s);
    if (it != index_.end()) {
        return it->second;
    }
    auto id = SymbolId(names_.size());
    names_.emplace_back(s);
    index_.emplace(names_.back(), id);
    return id;
}

SymbolTable::SymbolId SymbolTable::find(std::string_view s) const
{
    auto it = index_.find(s);
    return it == index_.end() ? INVALID : it->second;
}

}  // namespace statusbar::controlplane
