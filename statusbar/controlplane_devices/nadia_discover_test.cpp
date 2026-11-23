// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// discover_nadia_shape against a scripted fake device. The two designs
// mirror what real units serve: the RZ's loaded design (2026-09) answers
// `level` on populated strips and leaves `address` empty, while the
// pre-design behavior populated `address` — discovery must count either.

#include "statusbar/controlplane_devices/nadia_discover.hpp"

#include "statusbar/controlplane_devices/galaxy_client.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar::controlplane_devices;

TEST(nadia_discover, parses_design_level_shape_keys)
{
    NadiaShape shape;
    EXPECT_TRUE(parse_nadia_shape(
        "outputs=128,vras=12,vras_inputs=16,vras_returns=16,vras_outputs=8,reflections=16,monitors=24,agcs=16,master=1", shape));
    EXPECT_EQ(shape.vras, 12u);
    EXPECT_EQ(shape.vras_outputs, 8u);
    EXPECT_EQ(shape.monitors, 24u);
    EXPECT_TRUE(shape.master);
}

TEST_MAIN(statusbar_controlplane_devices, nadia_discover_test)
