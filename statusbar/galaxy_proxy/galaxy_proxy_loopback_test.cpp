// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The loopback proxy test (PLAN §9): the proxy runs against a fake GALAXY
// ASCII server so CI needs no hardware. Northbound AECP writes (driven at
// the handler level — no raw sockets) become southbound sets; southbound
// value reports become served control values.

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/galaxy_proxy/galaxy_proxy.hpp"
#include "statusbar/test/test.hpp"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::galaxy_proxy;

namespace {

/// A fake GALAXY: acks nothing, echoes every set as a value report (the
/// device's behavior for accepted sets), and lets the test inject
/// unsolicited value reports. Accepts connection after connection (a
/// reconnecting proxy comes back on a new socket) and records every
/// non-set command it is sent (gets, describes, subscribes).
struct FakeGalaxy
{
    int listen_fd = -1;
    std::atomic<int> conn_fd{-1};
    std::uint16_t port = 0;
    std::thread thread;
    std::mutex mutex;
    std::vector<std::string> received_sets;
    std::vector<std::string> received_commands;
    std::atomic<bool> echo_sets{true};  ///< the real device does not always echo
    std::atomic<bool> stop{false};

    bool start()
    {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listen_fd < 0 || ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listen_fd, 1) != 0) {
            return false;
        }
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        thread = std::thread([this] { serve(); });
        return true;
    }

    void serve()
    {
        while (!stop) {
            pollfd pfd{listen_fd, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0) {
                continue;
            }
            int fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            conn_fd = fd;
            std::string pending;
            char buf[4096];
            for (;;) {
                auto n = ::read(fd, buf, sizeof buf);
                if (n <= 0) {
                    break;
                }
                pending.append(buf, std::size_t(n));
                std::size_t at;
                while ((at = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, at);
                    pending.erase(0, at + 1);
                    handle(fd, line);
                }
            }
            conn_fd = -1;
            ::close(fd);
        }
    }

    void handle(int fd, std::string const& line)
    {
        if (line.empty()) {
            return;
        }
        auto eq = line.find('=');
        if (line[0] == '/' && eq != std::string::npos) {
            {
                std::lock_guard lock(mutex);
                received_sets.push_back(line);
            }
            // Echo the accepted set back as a value report.
            if (echo_sets) {
                auto echo = line + "\r\n";
                (void)!::write(fd, echo.data(), echo.size());
            }
            return;
        }
        // Gets/describes/subscribes/recalls: record, answer nothing.
        std::lock_guard lock(mutex);
        received_commands.push_back(line);
        if (line[0] == ':') {
            received_sets.push_back(line);
        }
    }

    void push(std::string const& value_report_line)
    {
        auto text = value_report_line + "\r\n";
        int fd = conn_fd;
        if (fd >= 0) {
            (void)!::write(fd, text.data(), text.size());
        }
    }

    std::vector<std::string> sets()
    {
        std::lock_guard lock(mutex);
        return received_sets;
    }

    std::size_t command_count(std::string const& line)
    {
        std::lock_guard lock(mutex);
        std::size_t n = 0;
        for (auto const& l : received_commands) {
            n += l == line;
        }
        return n;
    }

    ~FakeGalaxy()
    {
        stop = true;
        int fd = conn_fd;
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
        if (thread.joinable()) {
            thread.join();
        }
        if (listen_fd >= 0) {
            ::close(listen_fd);
        }
    }
};

std::vector<std::uint8_t> float_values(std::initializer_list<float> vs)
{
    std::vector<std::uint8_t> out;
    for (float v : vs) {
        std::array<std::uint8_t, 4> b{};
        detail::encode_linear_field(v, b);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

bool contains(std::vector<std::string> const& lines, std::string const& want)
{
    for (auto const& l : lines) {
        if (l == want) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(galaxy_proxy, loopback_mirror_both_directions)
{
    FakeGalaxy fake;
    EXPECT_TRUE(fake.start());

    GalaxyProxyConfig config;
    config.galaxy_host = "127.0.0.1";
    config.galaxy_port = fake.port;
    config.galaxy_entity_id = 0x001cabfffe00c8f0ull;
    GalaxyProxy proxy(config);

    // Identity derivation (PLAN §7.1): deterministic, JDKS-based.
    EXPECT_EQ(proxy.proxy_entity_id().get(), 0x70B3D5EDC200C8F0ull);

    EXPECT_TRUE(proxy.connect_southbound());
    std::uint64_t now = 1;

    // ---- northbound SET_CONTROL -> southbound set -------------------
    auto gain = proxy.ns().adaptor.resolve("/processing/input/1/gain");
    auto ref = proxy.aem().binding.address_of(*gain);
    EXPECT_EQ(ref.descriptor_type, std::uint16_t(DESCRIPTOR_CONTROL));

    nanoavb::DescriptorId id{{0, DESCRIPTOR_CONTROL, ref.descriptor_index}, 0};
    auto status = proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, id, float_values({-6.5f}));
    EXPECT_EQ(status, AEM_STATUS_SUCCESS);

    // The set went southbound; the fake echoed it; the echo lands in the
    // mirror and re-applies without a second southbound set. (The mirror
    // reports the point immediately — the optimistic local echo — so wait
    // on the fake's receipt, not on reported().)
    for (int i = 0; i < 50 && !contains(fake.sets(), "/processing/input/1/gain='-6.5'"); ++i) {
        proxy.poll(100, now++);
    }
    EXPECT_TRUE(contains(fake.sets(), "/processing/input/1/gain='-6.5'"));
    EXPECT_TRUE(proxy.mirror().value_of(*gain) == "-6.5");
    EXPECT_EQ(proxy.southbound_set_count(), 1u);

    // Served control value reflects the set.
    std::array<std::uint8_t, 8> out{};
    auto n = proxy.handler().on_get_descriptor_value(AEM_COMMAND_GET_CONTROL, id, {}, out);
    EXPECT_EQ(n, 4u);
    {
        std::array<std::uint8_t, 4> b{out[0], out[1], out[2], out[3]};
        EXPECT_EQ(detail::decode_linear_field<float>(b), -6.5f);
    }

    // ---- northbound bool + read-only rejection ----------------------
    auto mute = proxy.ns().adaptor.resolve("/processing/input/1/mute");
    auto mute_ref = proxy.aem().binding.address_of(*mute);
    nanoavb::DescriptorId mute_id{{0, DESCRIPTOR_CONTROL, mute_ref.descriptor_index}, 0};
    std::uint8_t off[1] = {0};  // schema default is muted; 0 = unmute
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, mute_id, off), AEM_STATUS_SUCCESS);
    for (int i = 0; i < 50 && !contains(fake.sets(), "/processing/input/1/mute='false'"); ++i) {
        proxy.poll(100, now++);
    }
    EXPECT_TRUE(contains(fake.sets(), "/processing/input/1/mute='false'"));

    auto used = proxy.ns().adaptor.resolve("/status/matrix_crosspoints_used");
    auto used_ref = proxy.aem().binding.address_of(*used);
    nanoavb::DescriptorId used_id{{0, DESCRIPTOR_CONTROL, used_ref.descriptor_index}, 0};
    EXPECT_EQ(
        proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, used_id, float_values({99.f})), AEM_STATUS_NOT_SUPPORTED);

    // ---- southbound read-only telemetry -> served GET state ----------
    // A mirrored meter report must land in the stored control state (the
    // self-apply bypasses the northbound read-only rejection above); the
    // meter bank is an 8-wide float array with no blob fallback, so GET
    // serves nothing until the state exists.
    auto meter1 = proxy.ns().adaptor.resolve("/status/meter/input/1");
    auto meter_ref = proxy.aem().binding.address_of(*meter1);
    EXPECT_EQ(meter_ref.descriptor_type, std::uint16_t(DESCRIPTOR_CONTROL));
    fake.push("/status/meter/input/1='-26.5'");
    fake.push("/status/meter/input/2='-40'");
    for (int i = 0; i < 50 && !proxy.mirror().reported().test(*meter1); ++i) {
        proxy.poll(100, now++);
    }
    proxy.poll(50, now++);
    nanoavb::DescriptorId meter_id{{0, DESCRIPTOR_CONTROL, meter_ref.descriptor_index}, 0};
    std::array<std::uint8_t, 64> meters{};
    auto meter_bytes = proxy.handler().on_get_descriptor_value(AEM_COMMAND_GET_CONTROL, meter_id, {}, meters);
    EXPECT_EQ(meter_bytes, 32u);  // 8 x float
    {
        std::array<std::uint8_t, 4> b0{meters[0], meters[1], meters[2], meters[3]};
        std::array<std::uint8_t, 4> b1{meters[4], meters[5], meters[6], meters[7]};
        EXPECT_EQ(detail::decode_linear_field<float>(b0), -26.5f);
        EXPECT_EQ(detail::decode_linear_field<float>(b1), -40.f);
    }
    // The self-apply stored state without sending anything southbound.
    EXPECT_FALSE(contains(fake.sets(), "/status/meter/input/1='-26.5'"));
    // A genuine northbound write to the meter bank is still rejected.
    EXPECT_EQ(
        proxy.handler().on_set_descriptor_value(
            AEM_COMMAND_SET_CONTROL, meter_id, float_values({0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f})),
        AEM_STATUS_NOT_SUPPORTED);

    // ---- snapshot recall becomes the built-in function --------------
    auto snap = proxy.ns().adaptor.resolve("/project/snapshot/active/id");
    auto snap_ref = proxy.aem().binding.address_of(*snap);
    nanoavb::DescriptorId snap_id{{0, DESCRIPTOR_CONTROL, snap_ref.descriptor_index}, 0};
    std::vector<std::uint8_t> id16;
    {
        std::array<std::uint8_t, 2> b{};
        detail::encode_linear_field(std::int16_t(3), b);
        id16.assign(b.begin(), b.end());
    }
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, snap_id, id16), AEM_STATUS_SUCCESS);
    for (int i = 0; i < 20 && !contains(fake.sets(), ":recall_snapshot 3"); ++i) {
        proxy.poll(50, now++);
    }
    EXPECT_TRUE(contains(fake.sets(), ":recall_snapshot 3"));

    // ---- southbound (Compass-side) change -> served value -----------
    fake.push("/processing/input/2/gain='-12'");
    auto gain2 = proxy.ns().adaptor.resolve("/processing/input/2/gain");
    for (int i = 0; i < 50 && !proxy.mirror().reported().test(*gain2); ++i) {
        proxy.poll(100, now++);
    }
    EXPECT_TRUE(proxy.mirror().value_of(*gain2) == "-12");
    auto ref2 = proxy.aem().binding.address_of(*gain2);
    nanoavb::DescriptorId id2{{0, DESCRIPTOR_CONTROL, ref2.descriptor_index}, 0};
    n = proxy.handler().on_get_descriptor_value(AEM_COMMAND_GET_CONTROL, id2, {}, out);
    EXPECT_EQ(n, 4u);
    {
        std::array<std::uint8_t, 4> b{out[0], out[1], out[2], out[3]};
        EXPECT_EQ(detail::decode_linear_field<float>(b), -12.f);
    }
    // The southbound change re-applied locally without a new southbound
    // set (echo suppression closed the loop).
    auto sets_after = proxy.southbound_set_count();
    proxy.poll(100, now++);
    EXPECT_EQ(proxy.southbound_set_count(), sets_after);

    // ---- northbound SET_MATRIX -> per-crosspoint sets ---------------
    nanoavb::DescriptorId matrix_id{{0, DESCRIPTOR_MATRIX, 0}, 0};
    // SET_MATRIX region payload (Clause 7.4.33, after the generic
    // descriptor type/index framing): matrix_column, matrix_row,
    // region_width, region_height, rep|direction|value_count, item_offset,
    // then values.
    std::vector<std::uint8_t> matrix_body;
    auto push16 = [&](std::uint16_t v) {
        matrix_body.push_back(std::uint8_t(v >> 8));
        matrix_body.push_back(std::uint8_t(v & 0xFF));
    };
    push16(4);  // column (crosspoint/output 5)
    push16(2);  // row (matrix input 3)
    push16(1);  // width
    push16(1);  // height
    push16(1);  // rep off, direction horizontal, value_count 1
    push16(0);  // item_offset
    auto cell = float_values({-3.f});
    matrix_body.insert(matrix_body.end(), cell.begin(), cell.end());
    auto matrix_status = proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_MATRIX, matrix_id, matrix_body);
    EXPECT_EQ(matrix_status, AEM_STATUS_SUCCESS);
    for (int i = 0; i < 20 && !contains(fake.sets(), "/processing/matrix/3/5/gain='-3'"); ++i) {
        proxy.poll(50, now++);
    }
    EXPECT_TRUE(contains(fake.sets(), "/processing/matrix/3/5/gain='-3'"));

    // ---- southbound (Compass-side) crosspoint -> served MATRIX ------
    // A mirrored crosspoint report re-applies into the served grid as a
    // 1x1 SET_MATRIX self-write, with nothing echoed back southbound.
    {
        auto cross = proxy.ns().adaptor.resolve("/processing/matrix/7/2/delay");
        EXPECT_TRUE(cross.has_value());
        auto const cross_ref = proxy.aem().binding.address_of(*cross);
        EXPECT_EQ(cross_ref.descriptor_type, std::uint16_t(DESCRIPTOR_MATRIX));
        auto const sets_before_cross = proxy.southbound_set_count();

        fake.push("/processing/matrix/7/2/delay='4800'");
        for (int i = 0; i < 50 && !proxy.mirror().reported().test(*cross); ++i) {
            proxy.poll(100, now++);
        }
        proxy.poll(50, now++);

        // Read the single cell back through GET_MATRIX.
        std::vector<std::uint8_t> request;
        auto req16 = [&](std::uint16_t v) {
            request.push_back(std::uint8_t(v >> 8));
            request.push_back(std::uint8_t(v & 0xFF));
        };
        req16(std::uint16_t(cross_ref.value_index % 16));  // column
        req16(std::uint16_t(cross_ref.value_index / 16));  // row
        req16(1);                                          // width
        req16(1);                                          // height
        req16(1);                                          // count 1
        req16(0);                                          // item_offset
        nanoavb::DescriptorId cross_id{{0, DESCRIPTOR_MATRIX, cross_ref.descriptor_index}, 0};
        std::array<std::uint8_t, 32> cross_out{};
        auto const cross_bytes = proxy.handler().on_get_descriptor_value(AEM_COMMAND_GET_MATRIX, cross_id, request, cross_out);
        EXPECT_EQ(cross_bytes, 16u);  // 12-byte region echo + one float
        {
            std::array<std::uint8_t, 4> b{cross_out[12], cross_out[13], cross_out[14], cross_out[15]};
            EXPECT_EQ(detail::decode_linear_field<float>(b), 4800.f);
        }
        EXPECT_EQ(proxy.southbound_set_count(), sets_before_cross);
    }

    // ---- input mode SIGNAL_SELECTOR: both directions ----------------
    // Northbound SET_SIGNAL_SELECTOR with the authored mode-4 (AVB)
    // source becomes a southbound mode set.
    nanoavb::DescriptorId sel_id{{0, DESCRIPTOR_SIGNAL_SELECTOR, 0}, 0};
    auto const avb_source = galaxy_proxy::galaxy_input_mode_source(4);
    EXPECT_EQ(
        proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_SIGNAL_SELECTOR, sel_id, make_const_span(avb_source)),
        AEM_STATUS_SUCCESS);
    for (int i = 0; i < 20 && !contains(fake.sets(), "/device/input/1/mode='4'"); ++i) {
        proxy.poll(50, now++);
    }
    EXPECT_TRUE(contains(fake.sets(), "/device/input/1/mode='4'"));

    // A source outside the authored list is refused.
    atdecc::aem::SignalSource bogus{};
    bogus.signal_type = DESCRIPTOR_AUDIO_CLUSTER;
    bogus.signal_index = 0;
    EXPECT_EQ(
        proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_SIGNAL_SELECTOR, sel_id, make_const_span(bogus)),
        AEM_STATUS_BAD_ARGUMENTS);

    // Southbound (Compass-side) mode report -> served selector state,
    // with no echo back southbound.
    auto const sel_sets_before = proxy.southbound_set_count();
    fake.push("/device/input/2/mode='2'");
    auto mode2 = proxy.ns().adaptor.resolve("/device/input/2/mode");
    for (int i = 0; i < 50 && !proxy.mirror().reported().test(*mode2); ++i) {
        proxy.poll(100, now++);
    }
    proxy.poll(50, now++);
    nanoavb::DescriptorId sel2_id{{0, DESCRIPTOR_SIGNAL_SELECTOR, 1}, 0};
    std::array<std::uint8_t, 8> sel_out{};
    auto const sel_bytes = proxy.handler().on_get_descriptor_value(AEM_COMMAND_GET_SIGNAL_SELECTOR, sel2_id, {}, sel_out);
    EXPECT_EQ(sel_bytes, 8u);  // source triple + reserved
    {
        atdecc::aem::SignalSource served{};
        span_load(served, std::span<std::uint8_t const>{sel_out}.first(6));
        EXPECT_TRUE(served == galaxy_proxy::galaxy_input_mode_source(2));
    }
    EXPECT_EQ(proxy.southbound_set_count(), sel_sets_before);
}

TEST(galaxy_proxy, toggle_without_device_echo_reaches_southbound)
{
    // Live incident (2026-09-01): the venue GALAXY does not push every
    // self-inflicted change back on the proxy's connection. With the
    // mirror stale at the pre-mute report, the unmute equaled the stale
    // mirrored value and was suppressed — the widget showed unmuted while
    // the device stayed muted. The proxy now records its own sent sets in
    // the mirror (optimistic local echo), so a toggle round trip must
    // reach the device even when the device echoes nothing.
    FakeGalaxy fake;
    EXPECT_TRUE(fake.start());

    GalaxyProxyConfig config;
    config.galaxy_host = "127.0.0.1";
    config.galaxy_port = fake.port;
    config.galaxy_entity_id = 0x001cabfffe00c8f0ull;
    GalaxyProxy proxy(config);
    EXPECT_TRUE(proxy.connect_southbound());
    std::uint64_t now = 1;

    auto mute = proxy.ns().adaptor.resolve("/processing/input/2/mute");
    auto ref = proxy.aem().binding.address_of(*mute);
    nanoavb::DescriptorId id{{0, DESCRIPTOR_CONTROL, ref.descriptor_index}, 0};

    // Wait for the fake's accept before pushing (push writes to conn_fd).
    for (int i = 0; i < 100 && fake.conn_fd < 0; ++i) {
        proxy.poll(20, now++);
    }
    EXPECT_TRUE(fake.conn_fd >= 0);

    // The startup dump reported unmuted; after that the device goes quiet.
    fake.push("/processing/input/2/mute='false'");
    for (int i = 0; i < 50 && !proxy.mirror().reported().test(*mute); ++i) {
        proxy.poll(100, now++);
    }
    EXPECT_TRUE(proxy.mirror().reported().test(*mute));
    fake.echo_sets = false;

    std::uint8_t on[1] = {255};
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, id, on), AEM_STATUS_SUCCESS);
    for (int i = 0; i < 20 && !contains(fake.sets(), "/processing/input/2/mute='true'"); ++i) {
        proxy.poll(50, now++);
    }
    EXPECT_TRUE(contains(fake.sets(), "/processing/input/2/mute='true'"));

    // No echo arrived; the unmute must still go southbound.
    std::uint8_t off[1] = {0};
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, id, off), AEM_STATUS_SUCCESS);
    for (int i = 0; i < 20 && !contains(fake.sets(), "/processing/input/2/mute='false'"); ++i) {
        proxy.poll(50, now++);
    }
    EXPECT_TRUE(contains(fake.sets(), "/processing/input/2/mute='false'"));

    // And writing the (locally known) current value back stays suppressed —
    // the safe-SET discipline live tests rely on.
    auto const sets_before = proxy.southbound_set_count();
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, id, off), AEM_STATUS_SUCCESS);
    EXPECT_EQ(proxy.southbound_set_count(), sets_before);
}

TEST(galaxy_proxy, silent_device_is_detected_and_reconnect_requests_state_again)
{
    // Live incident (2026-10-13): a repatch moved the GALAXY off the
    // proxy's segment without a FIN or RST, and the proxy sat "connected"
    // to a silent socket for four days, serving stale state, until a
    // manual restart. The pump must notice silence by itself, declare the
    // link lost, and a reconnect must redo the startup state request.
    FakeGalaxy fake;
    EXPECT_TRUE(fake.start());

    GalaxyProxyConfig config;
    config.galaxy_host = "127.0.0.1";
    config.galaxy_port = fake.port;
    config.galaxy_entity_id = 0x001cabfffe00c8f0ull;
    config.southbound_quiet_ms = 1;
    config.southbound_ping_timeout_ms = 1;
    GalaxyProxy proxy(config);
    EXPECT_TRUE(proxy.connect_southbound());
    constexpr std::uint64_t MS = 1'000'000;
    std::uint64_t now = 1000 * MS;

    for (int i = 0; i < 100 && fake.command_count("/processing/.*") == 0; ++i) {
        proxy.poll(20, now);
    }
    EXPECT_EQ(fake.command_count("/processing/.*"), 1u);

    // The device answers nothing: after the quiet period the proxy pings
    // it (a describe — never a value report, so nothing is re-served).
    EXPECT_EQ(proxy.poll(0, now), 0);
    now += 2 * MS;
    EXPECT_EQ(proxy.poll(0, now), 0);
    EXPECT_EQ(proxy.client().ping_count(), 1u);
    for (int i = 0; i < 100 && fake.command_count("?/status/model_string") == 0; ++i) {
        usleep(10000);
    }
    EXPECT_EQ(fake.command_count("?/status/model_string"), 1u);

    // Still nothing: the link is declared lost.
    now += 2 * MS;
    EXPECT_EQ(proxy.poll(0, now), -1);
    EXPECT_FALSE(proxy.client().connected());
    EXPECT_TRUE(proxy.client().error().find("no reply to liveness ping") != std::string::npos);
    // ... and stays lost until reconnected.
    EXPECT_EQ(proxy.poll(0, now), -1);

    // The tool's recovery: reconnect == the full startup sequence again.
    EXPECT_TRUE(proxy.connect_southbound());
    // The startup burst is the gets followed by the subscribes; wait for the
    // subscribe, which the fake's reader sees last.
    for (int i = 0; i < 100 && fake.command_count("+/processing/.* 100") < 2; ++i) {
        proxy.poll(20, now);
    }
    EXPECT_EQ(fake.command_count("/processing/.*"), 2u);
    EXPECT_EQ(fake.command_count("+/processing/.* 100"), 2u);
    // The new connection's quiet clock starts at its first poll.
    EXPECT_EQ(proxy.poll(0, now), 0);

    // A device that answers the ping stays connected: the quiet clock
    // restarts from its reply and the mirror still takes the report.
    now += 2 * MS;
    EXPECT_EQ(proxy.poll(0, now), 0);
    EXPECT_EQ(proxy.client().ping_count(), 2u);
    for (int i = 0; i < 100 && fake.command_count("?/status/model_string") < 2; ++i) {
        usleep(10000);
    }
    fake.push("/processing/input/1/gain='-3.5'");
    auto gain = proxy.ns().adaptor.resolve("/processing/input/1/gain");
    for (int i = 0; i < 50 && !proxy.mirror().reported().test(*gain); ++i) {
        proxy.poll(20, now);
    }
    EXPECT_TRUE(proxy.mirror().reported().test(*gain));
    now += 2 * MS;
    EXPECT_EQ(proxy.poll(0, now), 0);
    EXPECT_TRUE(proxy.client().connected());
}

//
// Test Runner
//

TEST_MAIN(statusbar_galaxy_proxy, galaxy_proxy_loopback_test)
