// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane_bitset.hpp"

#include <cstdint>
#include <memory>
#include <vector>

// The static, shared, immutable half of the control plane (docs/PLAN.md
// §4.2): one Model per entity_model_id, backing any number of Instances.
// Storage is structure-of-arrays indexed by ControlPointId; handles are
// assigned by insertion order into the builder, which must be a
// deterministic canonical walk of the model so handles are stable across
// process restarts and machines (§4.1). SymbolTable and Fingerprint arrive
// with the addressing milestone (M2).

namespace statusbar::controlplane {

enum class PointFlags : std::uint8_t
{
    none = 0,
    read_only = 0x01,
    volatile_value = 0x02,
};

constexpr PointFlags operator|(PointFlags a, PointFlags b)
{
    return PointFlags(std::uint8_t(a) | std::uint8_t(b));
}

constexpr bool has_flag(PointFlags flags, PointFlags f)
{
    return (std::uint8_t(flags) & std::uint8_t(f)) != 0;
}

/// Projection of a control point into the AEM address space:
/// (configuration, descriptor_type, descriptor_index, value_index).
/// Plain data — the binding logic lives in controlplane_aem_binding.hpp.
struct AemRef
{
    static constexpr std::uint16_t NO_DESCRIPTOR = 0xffff;

    std::uint16_t configuration = 0;
    std::uint16_t descriptor_type = NO_DESCRIPTOR;
    std::uint16_t descriptor_index = 0;
    std::uint16_t value_index = 0;

    friend bool operator==(AemRef const&, AemRef const&) = default;
};

/// Builder input: everything the model records per control point.
struct ControlPointDesc
{
    static constexpr std::uint16_t NO_LOCALIZED = 0xffff;

    std::uint64_t control_type = 0;  ///< EUI-64
    std::uint8_t value_type = 0;     ///< CONTROL_LINEAR_* / ARRAY / SELECTOR / UTF8
    std::uint16_t units = 0;         ///< UNIT_CODE_*
    std::int8_t multiplier = 0;      ///< power of 10
    double min = 0.0;
    double max = 0.0;
    double step = 0.0;
    double def = 0.0;
    std::uint16_t localized = NO_LOCALIZED;  ///< packed (strings_index << 3) | string_index
    PointFlags flags = PointFlags::none;
    AemRef aem{};
};

class Model
{
  public:
    std::size_t size() const noexcept { return control_type_.size(); }

    std::uint64_t control_type(ControlPointId h) const { return control_type_[h]; }
    std::uint8_t value_type(ControlPointId h) const { return value_type_[h]; }
    std::uint16_t units(ControlPointId h) const { return units_[h]; }
    std::int8_t multiplier(ControlPointId h) const { return multiplier_[h]; }
    double min(ControlPointId h) const { return min_[h]; }
    double max(ControlPointId h) const { return max_[h]; }
    double step(ControlPointId h) const { return step_[h]; }
    double def(ControlPointId h) const { return def_[h]; }
    std::uint16_t localized(ControlPointId h) const { return localized_[h]; }
    PointFlags flags(ControlPointId h) const { return flags_[h]; }
    AemRef const& aem(ControlPointId h) const { return aem_[h]; }

    bool read_only(ControlPointId h) const { return has_flag(flags_[h], PointFlags::read_only); }

  private:
    friend class ModelBuilder;

    std::vector<std::uint64_t> control_type_;
    std::vector<std::uint8_t> value_type_;
    std::vector<std::uint16_t> units_;
    std::vector<std::int8_t> multiplier_;
    std::vector<double> min_, max_, step_, def_;
    std::vector<std::uint16_t> localized_;
    std::vector<PointFlags> flags_;
    std::vector<AemRef> aem_;
};

/// Interns control points in canonical-walk order and produces the immutable
/// shared Model. build() leaves the builder empty and reusable.
class ModelBuilder
{
  public:
    ControlPointId add(ControlPointDesc const& d);
    std::shared_ptr<Model const> build();

    std::size_t size() const noexcept { return model_.size(); }

  private:
    Model model_;
};

}  // namespace statusbar::controlplane
