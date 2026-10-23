// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

// Build version baked into the galaxy_proxy entity's ENTITY descriptor
// firmware_version. The build injects STATUSBAR_ATDECC_CONTROL_VERSION from
// the repo release tag (see CMakeLists.txt's version block); a build without
// it gets an obvious dev marker rather than a plausible-but-wrong number.
#ifndef STATUSBAR_ATDECC_CONTROL_VERSION
#    define STATUSBAR_ATDECC_CONTROL_VERSION "0.0.0-dev"
#endif

namespace statusbar::controlplane {

constexpr std::string_view version() noexcept
{
    return STATUSBAR_ATDECC_CONTROL_VERSION;
}

}  // namespace statusbar::controlplane
