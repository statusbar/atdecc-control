// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane_bitset.hpp"
#include "statusbar/controlplane/controlplane_symbols.hpp"
#include "statusbar/controlplane/controlplane_trie.hpp"

#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Address adaptors (docs/PLAN.md §4.8). Addresses are projections, not
// identity: the AEM tuple and the device path (GALAXY ASCII, Nadia) are
// independent indexes into one handle space. Every adaptor offers the same
// three operations; PathAdaptor is the slash-separated-path implementation
// that the GALAXY and Nadia adaptors wrap, while the AEM binding
// (controlplane_aem_binding.hpp) binds the AemRef tuple its own way.

namespace statusbar::controlplane {

/// The adaptor contract (the original SchemaAdaptor<AddressT> shape).
template <typename A, typename AddressT>
concept AddressAdaptor = requires(A const a, AddressT const& addr, std::string_view pattern, ControlPointId h) {
    { a.resolve(addr) } -> std::same_as<std::optional<ControlPointId>>;
    { a.resolve_pattern(pattern) } -> std::same_as<Selector>;
    { a.address_of(h) } -> std::convertible_to<AddressT>;
};

class PathAdaptorBuilder;

/// Immutable path -> handle index: exact resolution walks the frozen
/// segment-trie image (one interned-symbol binary search per segment);
/// patterns compile once, at subscribe time, into a Selector bitset.
/// Pattern segments: `*` matches any one segment, `#` matches any numeric
/// segment, anything else is a literal.
///
/// The trie is a core FlatTrie image owned by this adaptor: pointer-free
/// bytes queried through a view, so the same image can later come from a
/// read-only mapped model store instead.
class PathAdaptor
{
  public:
    PathAdaptor() = default;

    // The view borrows trie_image_'s heap block: moves keep it (and the
    // view) valid; copies must re-attach to their own copy of the bytes.
    PathAdaptor(PathAdaptor&&) noexcept = default;
    PathAdaptor& operator=(PathAdaptor&&) noexcept = default;
    PathAdaptor(PathAdaptor const& other)
        : symbols_(other.symbols_)
        , trie_image_(other.trie_image_)
        , paths_(other.paths_)
    {
        attach();
    }
    PathAdaptor& operator=(PathAdaptor const& other)
    {
        if (this != &other) {
            symbols_ = other.symbols_;
            trie_image_ = other.trie_image_;
            paths_ = other.paths_;
            attach();
        }
        return *this;
    }
    ~PathAdaptor() = default;

    /// Number of handles in the address space (the Selector size).
    std::size_t handle_space() const noexcept { return paths_.size(); }

    std::optional<ControlPointId> resolve(std::string_view path) const;

    /// Every handle whose path matches the pattern; empty Selector when the
    /// pattern uses symbols this address space has never seen.
    Selector resolve_pattern(std::string_view pattern) const;

    /// The canonical path mapped to h; empty when h is unmapped.
    std::string const& address_of(ControlPointId h) const { return paths_[h]; }

  private:
    friend class PathAdaptorBuilder;

    void attach()
    {
        auto view = SegmentTrieView::attach(trie_image_);
        trie_ = view ? *view : SegmentTrieView{};
    }

    SymbolTable symbols_;
    std::vector<std::byte> trie_image_;  ///< the frozen FlatTrie image
    SegmentTrieView trie_;               ///< borrows trie_image_
    std::vector<std::string> paths_;     ///< indexed by handle
};

static_assert(AddressAdaptor<PathAdaptor, std::string>);

/// Interns paths and produces the immutable PathAdaptor. handle_space is
/// the model's size(); every mapped handle must be below it.
class PathAdaptorBuilder
{
  public:
    explicit PathAdaptorBuilder(std::size_t handle_space) { adaptor_.paths_.resize(handle_space); }

    /// False when the path is already mapped (first mapping wins), the
    /// handle already has a path, or the handle is out of range.
    bool map(std::string_view path, ControlPointId h);

    /// Freezes the trie into the adaptor's image and returns the adaptor.
    /// Leaves the builder empty and reusable.
    PathAdaptor build();

  private:
    PathAdaptor adaptor_;
    SegmentTrie trie_;  ///< the growable build phase
};

}  // namespace statusbar::controlplane
