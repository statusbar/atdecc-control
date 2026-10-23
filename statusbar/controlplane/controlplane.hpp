// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane_adaptor.hpp"
#include "statusbar/controlplane/controlplane_bitset.hpp"
#include "statusbar/controlplane/controlplane_instance.hpp"
#include "statusbar/controlplane/controlplane_model.hpp"
#include "statusbar/controlplane/controlplane_scene.hpp"
#include "statusbar/controlplane/controlplane_symbols.hpp"
#include "statusbar/controlplane/controlplane_trie.hpp"
#include "statusbar/controlplane/controlplane_version.hpp"

#include <bit>
#include <cstdint>

// Umbrella header for the controlplane core (docs/PLAN.md §4). This module
// must never depend on statusbar-avb — the AEM binding lives in
// the AEM binding adaptor, which itself has no AVB dependency — so the core stays usable in a non-AVB device.

namespace statusbar::controlplane {

// Scalar values live in the flat Instance arrays as raw 64-bit patterns;
// these punning helpers are the one blessed conversion for double-typed
// control points.

constexpr std::uint64_t value_bits(double v) noexcept
{
    return std::bit_cast<std::uint64_t>(v);
}

constexpr double value_double(std::uint64_t bits) noexcept
{
    return std::bit_cast<double>(bits);
}

}  // namespace statusbar::controlplane
