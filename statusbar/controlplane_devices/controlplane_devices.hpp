// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane.hpp"

// Device adaptors: the GALAXY ASCII and Nadia text protocols, the folded
// device schemas that map their control-point paths onto the dense
// ControlPointId handle space, and the southbound transport state and
// mirrors (docs/PLAN.md §4.8, §7.3). The GALAXY's OSC ports are not used:
// the ASCII protocol covers the whole namespace.

namespace statusbar::controlplane_devices {}  // namespace statusbar::controlplane_devices
