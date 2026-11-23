// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The M8 loopback acceptance test (docs/PLAN.md §12): the bridge runs on
// a FAKE ControllerService serving a synthetic 3-control entity, and a
// real WebSocket client on the loopback HTTP server sees the whole
// story — widget specs out, a SET round-tripped through the (fake) AEM
// layer, an unsolicited change pushed live, and the departure. No AVB
// network, no hardware.

#include "statusbar/widget_bridge/widget_bridge.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_control_accessors.hpp"
#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/widget_bridge/offline_service.hpp"
#include "statusbar/widget_bridge/widget_json.hpp"

#include <poll.h>
#include <unistd.h>

#include <array>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::atdecc_tools;
using namespace statusbar::widget_bridge;
using ieee::Eui64;

namespace {

// ---- the synthetic entity model --------------------------------------------

// ---- the fake backend -------------------------------------------------------

// ---- WS test client ---------------------------------------------------------

// ---- harness ----------------------------------------------------------------

}  // namespace

// A JOYNED MU16 answers GET/SET_CONTROL for its 1 x UINT8 IDENTIFY with a
// 13-byte body (value + zero padding); the decoder must take the leading
// element and ignore the tail, and still reject a body that is too short.
TEST(widget_bridge, decode_tolerates_padded_control_body)
{
    DescriptorControl d{};
    d.control_value_type = pack_control_value_type(false, false, CONTROL_LINEAR_UINT8);
    d.number_of_values = 1;
    std::array<uint8_t, 13> padded{0xFF};
    auto const values = decode_control_values(d, make_const_span(padded));
    EXPECT_TRUE(values.has_value() && values->size() == 1 && (*values)[0] == 255.0);

    d.number_of_values = 2;
    std::array<uint8_t, 1> short_body{0x01};
    EXPECT_TRUE(!decode_control_values(d, make_const_span(short_body)).has_value());
    std::array<uint8_t, 3> two_plus_pad{0x01, 0x02, 0x00};
    auto const two = decode_control_values(d, make_const_span(two_plus_pad));
    EXPECT_TRUE(two.has_value() && two->size() == 2 && (*two)[0] == 1.0 && (*two)[1] == 2.0);
}

//
// Test Runner
//

TEST_MAIN(statusbar_widget_bridge, widget_bridge_test)
