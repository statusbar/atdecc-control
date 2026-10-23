// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane/controlplane_aem_binding.hpp"

#include <algorithm>
#include <tuple>

namespace statusbar::controlplane {

namespace {

constexpr auto ref_key(AemRef const& r)
{
    return std::tuple(r.configuration, r.descriptor_type, r.descriptor_index, r.value_index);
}

constexpr bool ref_less(std::pair<AemRef, ControlPointId> const& a, AemRef const& b)
{
    return ref_key(a.first) < ref_key(b);
}

}  // namespace

std::optional<ControlPointId> AemBinding::resolve(AemRef const& ref) const
{
    auto it = std::lower_bound(forward_.begin(), forward_.end(), ref, ref_less);
    if (it == forward_.end() || !(it->first == ref)) {
        return std::nullopt;
    }
    return it->second;
}

controlplane::Selector AemBinding::values_of(std::uint16_t descriptor_type, std::uint16_t descriptor_index) const
{
    controlplane::Selector out(reverse_.size());
    AemRef first{0, descriptor_type, descriptor_index, 0};
    for (auto it = std::lower_bound(forward_.begin(), forward_.end(), first, ref_less);
         it != forward_.end() && it->first.descriptor_type == descriptor_type && it->first.descriptor_index == descriptor_index;
         ++it) {
        out.set(it->second);
    }
    return out;
}

bool AemBindingBuilder::bind(AemRef const& ref, ControlPointId h)
{
    if (h >= binding_.reverse_.size() || binding_.reverse_[h].descriptor_type != AemRef::NO_DESCRIPTOR) {
        return false;
    }
    auto it = std::lower_bound(binding_.forward_.begin(), binding_.forward_.end(), ref, ref_less);
    if (it != binding_.forward_.end() && it->first == ref) {
        return false;
    }
    binding_.forward_.insert(it, {ref, h});
    binding_.reverse_[h] = ref;
    return true;
}

AemBinding AemBindingBuilder::build()
{
    AemBinding out = std::move(binding_);
    binding_ = AemBinding{};
    binding_.reverse_.assign(out.reverse_.size(), AemRef{});
    return out;
}

}  // namespace statusbar::controlplane
