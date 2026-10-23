// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the controlplane core module.

#include "statusbar/controlplane/controlplane.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;

TEST(controlplane, version_is_nonempty)
{
    EXPECT_TRUE(!controlplane::version().empty());
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane, controlplane_test)
