// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane_devices/galaxy_schema.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Mirrors a live GALAXY's control-point state onto the controlplane core:
// value-report lines feed the Instance (controller role — the device is
// always the authority) and a per-handle string store, since M1's Instance
// carries scalars only. Reconciliation against the generated schema falls
// out for free: paths the device reports that the schema lacks are
// collected as unknown, and schema points the device never reported stay
// out of the reported set (docs/PLAN.md §6.4's matched/missing/unmatched
// shape, applied to the namespace itself).

namespace statusbar::controlplane_devices {

class GalaxyMirror
{
  public:
    /// Uses the generated GALAXY 816 namespace.
    GalaxyMirror();

    /// Uses a caller-built namespace (shared with other projections of the
    /// same handle space, e.g. the proxy's AEM binding).
    explicit GalaxyMirror(GalaxyNamespace ns);

    GalaxyNamespace const& ns() const noexcept { return ns_; }
    controlplane::Instance const& instance() const noexcept { return instance_; }

    /// Consumes one response line. Value reports update the mirror;
    /// describe/other lines are counted and otherwise ignored here.
    /// Returns the handle for value reports of known paths.
    std::optional<controlplane::ControlPointId> consume_line(std::string_view line, std::uint64_t now_ns);

    /// Record a value this side just wrote to the device — the optimistic
    /// local echo. The mirror answers "what is the device state" and a
    /// sent set is part of that state even before (or without) the device
    /// reporting it back: the GALAXY does not push every self-inflicted
    /// change on the set connection, and comparing against a stale report
    /// made the proxy suppress the write that would undo a previous one
    /// (a mute whose unmute never went southbound). A later device report
    /// for the point simply overwrites this.
    void record_local_set(controlplane::ControlPointId h, std::string_view value, std::uint64_t now_ns);

    /// The live value string of a reported point.
    std::string const& value_of(controlplane::ControlPointId h) const { return values_[h]; }

    /// Points that have reported at least once.
    controlplane::Bitset const& reported() const noexcept { return reported_; }

    /// Schema points the device has not reported (schema minus reported).
    controlplane::Bitset missing() const;

    /// Paths the device reported that the schema does not know, in first-
    /// seen order, deduplicated.
    std::vector<std::string> const& unknown_paths() const noexcept { return unknown_paths_; }

    std::size_t value_report_count() const noexcept { return value_report_count_; }
    std::size_t describe_count() const noexcept { return describe_count_; }
    std::size_t other_count() const noexcept { return other_count_; }

  private:
    GalaxyNamespace ns_;
    controlplane::Instance instance_;
    std::vector<std::string> values_;
    controlplane::Bitset reported_;
    std::vector<std::string> unknown_paths_;
    std::size_t value_report_count_ = 0;
    std::size_t describe_count_ = 0;
    std::size_t other_count_ = 0;
};

}  // namespace statusbar::controlplane_devices
