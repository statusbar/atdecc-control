// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the controlplane_devices adaptor module.

#include "statusbar/controlplane_devices/controlplane_devices.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;

TEST(controlplane_devices, module_links)
{
    EXPECT_TRUE(!controlplane::version().empty());
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane_devices, controlplane_devices_test)
