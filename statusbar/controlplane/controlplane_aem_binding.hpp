// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane_bitset.hpp"
#include "statusbar/controlplane/controlplane_model.hpp"

#include <optional>
#include <vector>

// The AEM projection of the handle space (docs/PLAN.md §2, §4.8):
// (configuration, descriptor_type, descriptor_index, value_index) tuples
// bound to ControlPointIds. Addresses are projections, not identity — this
// is one more index into the same handle space the device path adaptor
// indexes. Pattern resolution has no AEM counterpart (AECP addresses one
// descriptor at a time), so this adaptor offers resolve/address_of plus a
// per-descriptor Selector.

namespace statusbar::controlplane {

class AemBindingBuilder;

class AemBinding
{
  public:
    AemBinding() = default;

    std::optional<ControlPointId> resolve(AemRef const& ref) const;

    /// The AemRef bound to h; descriptor_type == AemRef::NO_DESCRIPTOR
    /// when the handle has no AEM projection.
    AemRef const& address_of(ControlPointId h) const { return reverse_[h]; }

    /// Every handle bound to values of one descriptor.
    controlplane::Selector values_of(std::uint16_t descriptor_type, std::uint16_t descriptor_index) const;

    std::size_t handle_space() const noexcept { return reverse_.size(); }
    std::size_t bound_count() const noexcept { return forward_.size(); }

  private:
    friend class AemBindingBuilder;

    std::vector<std::pair<AemRef, ControlPointId>> forward_;  ///< sorted by ref
    std::vector<AemRef> reverse_;                             ///< indexed by handle
};

class AemBindingBuilder
{
  public:
    explicit AemBindingBuilder(std::size_t handle_space) { binding_.reverse_.assign(handle_space, AemRef{}); }

    /// False when the ref or the handle is already bound, or the handle is
    /// out of range. One handle carries at most one AEM projection.
    bool bind(AemRef const& ref, ControlPointId h);

    AemBinding build();

  private:
    AemBinding binding_;
};

}  // namespace statusbar::controlplane
