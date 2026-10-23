// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane_devices/galaxy_client.hpp"
#include "statusbar/controlplane_devices/nadia_schema.hpp"

#include <cstdio>
#include <string_view>

// Shape (channel-count) handling shared by the Nadia tools: the
// `--shape k=v,...` option parser and the read-only live probe that counts
// a loaded design's channels over the text-command port.

namespace statusbar::controlplane_devices {

/// Parses "inputs=16,buses=8,outputs=16,listens=0,vgroups=0,bands=8"
/// (also "eq_bands", "system=0|1") into @p shape. False on a bad item.
bool parse_nadia_shape(std::string_view text, NadiaShape& shape);

/// Probes the loaded design's channel counts with read-only gets of the
/// per-channel "address" points (channels the design does not declare
/// answer empty or as unknown addresses) and the parametric band count
/// from input 1; @p max caps each family's probe range. Logs the result
/// to @p log when non-null.
NadiaShape discover_nadia_shape(GalaxyClient& client, unsigned max, FILE* log);

}  // namespace statusbar::controlplane_devices
