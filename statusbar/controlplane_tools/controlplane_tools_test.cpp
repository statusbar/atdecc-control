// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the controlplane_tools module.

#include "statusbar/controlplane_tools/controlplane_tools.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;

TEST(controlplane_tools, module_links)
{
    EXPECT_TRUE(!controlplane::version().empty());
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane_tools, controlplane_tools_test)
