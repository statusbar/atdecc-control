// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane_devices/nadia_schema.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Mirrors a live Nadia CP's control-point state onto the controlplane
// core — the GalaxyMirror discipline (the device is always the
// authority; the Instance holds scalars, a per-handle string store holds
// the verbatim text) over the Core text protocol: "Got: Input 1 Level =
// -3.6" lines resolve by friendly name. Names the device reports that
// the namespace lacks are collected as unknown; namespace points the
// device never answered stay out of the reported set.

namespace statusbar::controlplane_devices {

class NadiaMirror
{
  public:
    explicit NadiaMirror(NadiaNamespace ns);

    NadiaNamespace const& ns() const noexcept { return ns_; }
    controlplane::Instance const& instance() const noexcept { return instance_; }

    /// Consumes one response line. Value reports update the mirror;
    /// anything else is counted. Returns the handle for reports of known
    /// names. An empty value (the address exists but the design does not
    /// populate it) is recorded as reported-but-empty.
    std::optional<controlplane::ControlPointId> consume_line(std::string_view line, std::uint64_t now_ns);

    /// Record a value this side just wrote — the optimistic local echo
    /// (see GalaxyMirror::record_local_set for why).
    void record_local_set(controlplane::ControlPointId h, std::string_view value, std::uint64_t now_ns);

    /// The live value text of a reported point.
    std::string const& value_of(controlplane::ControlPointId h) const { return values_[h]; }

    /// The scalar projection of a reported point, nullopt for text and
    /// empty values.
    std::optional<double> scalar_of(controlplane::ControlPointId h) const;

    /// Points that have reported at least once (empty values included).
    controlplane::Bitset const& reported() const noexcept { return reported_; }

    /// Points that have reported a non-empty value: what the loaded
    /// design actually populates.
    controlplane::Bitset const& populated() const noexcept { return populated_; }

    /// Namespace points the device has not reported.
    controlplane::Bitset missing() const;

    std::vector<std::string> const& unknown_names() const noexcept { return unknown_names_; }

    std::size_t value_report_count() const noexcept { return value_report_count_; }
    std::size_t unknown_address_count() const noexcept { return unknown_address_count_; }
    std::size_t other_count() const noexcept { return other_count_; }

  private:
    void store(controlplane::ControlPointId h, std::string_view value, std::uint64_t now_ns);

    NadiaNamespace ns_;
    controlplane::Instance instance_;
    std::vector<std::string> values_;
    controlplane::Bitset reported_;
    controlplane::Bitset populated_;
    std::vector<std::string> unknown_names_;
    std::size_t value_report_count_ = 0;
    std::size_t unknown_address_count_ = 0;
    std::size_t other_count_ = 0;
};

}  // namespace statusbar::controlplane_devices
