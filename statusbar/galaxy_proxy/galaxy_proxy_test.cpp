// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the galaxy_proxy entity module.

#include "statusbar/galaxy_proxy/galaxy_proxy.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;

TEST(galaxy_proxy, module_links)
{
    EXPECT_TRUE(!controlplane::version().empty());
}

//
// Test Runner
//

TEST_MAIN(statusbar_galaxy_proxy, galaxy_proxy_test)
