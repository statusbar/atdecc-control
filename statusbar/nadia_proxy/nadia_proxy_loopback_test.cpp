// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The loopback proxy test: the proxy runs against a fake Nadia CP text
// server so CI needs no hardware. Northbound AECP writes (driven at the
// handler level — no raw sockets) become southbound sets; southbound
// value reports become served control values.

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/controlplane_devices/nadia_ascii.hpp"
#include "statusbar/nadia_proxy/nadia_proxy.hpp"
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
using namespace statusbar::nadia_proxy;

namespace {

/// A fake Core: answers gets/subscribes with nothing (the test pushes
/// reports itself), echoes every set as a `Got:` report the way the
/// device does for accepted sets, and records what it received. Accepts
/// connection after connection (a reconnecting proxy comes back on a new
/// socket).
struct FakeNadia
{
    int listen_fd = -1;
    std::atomic<int> conn_fd{-1};
    std::uint16_t port = 0;
    std::thread thread;
    std::mutex mutex;
    std::vector<std::string> received;
    std::vector<std::string> received_commands;  ///< every non-set line
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
        if (!line.starts_with("set ")) {
            std::lock_guard lock(mutex);
            received_commands.push_back(line);
            return;
        }
        {
            std::lock_guard lock(mutex);
            received.push_back(line);
        }
        // "set input 1 level = -6.5" -> "Got: Input 1 Level = -6.5"
        auto eq = line.find(" = ");
        if (eq == std::string::npos) {
            return;
        }
        std::string path = "/" + line.substr(4, eq - 4);
        for (auto& ch : path) {
            if (ch == ' ') {
                ch = '/';
            }
        }
        auto echo = "Got: " + controlplane_devices::nadia_friendly_name(path) + " = " + line.substr(eq + 3) + "\r\n";
        (void)!::write(fd, echo.data(), echo.size());
    }

    void push(std::string const& report_line)
    {
        auto text = report_line + "\r\n";
        int fd = conn_fd;
        if (fd >= 0) {
            (void)!::write(fd, text.data(), text.size());
        }
    }

    std::vector<std::string> sets()
    {
        std::lock_guard lock(mutex);
        return received;
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

    ~FakeNadia()
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

std::vector<std::uint8_t> int64_values(std::initializer_list<std::int64_t> vs)
{
    std::vector<std::uint8_t> out;
    for (auto v : vs) {
        std::array<std::uint8_t, 8> b{};
        detail::encode_linear_field(v, b);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

float float_at(std::span<std::uint8_t const> bytes, std::size_t i)
{
    std::array<std::uint8_t, 4> b{};
    std::memcpy(b.data(), bytes.data() + 4 * i, 4);
    return detail::decode_linear_field<float>(b);
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

controlplane_devices::NadiaShape small_shape()
{
    controlplane_devices::NadiaShape s;
    s.inputs = 4;
    s.buses = 2;
    s.outputs = 2;
    s.eq_bands = 2;
    return s;
}

}  // namespace

TEST(nadia_proxy, loopback_mirror_both_directions)
{
    FakeNadia fake;
    EXPECT_TRUE(fake.start());

    NadiaProxyConfig config;
    config.nadia_host = "127.0.0.1";
    config.nadia_port = fake.port;
    config.nadia_entity_id = 0x000732fffe959361ull;
    config.shape = small_shape();
    NadiaProxy proxy(config);

    // Identity derivation: deterministic, JDKS-based, distinct from the
    // GALAXY proxy's bank.
    EXPECT_EQ(proxy.proxy_entity_id().get(), 0x70B3D5EDC4959361ull);

    EXPECT_TRUE(proxy.connect_southbound());
    std::uint64_t now = 1;

    auto control_of = [&](char const* path) {
        auto ref = proxy.aem().binding.address_of(*proxy.ns().resolve_path(path));
        EXPECT_EQ(ref.descriptor_type, std::uint16_t(DESCRIPTOR_CONTROL));
        return nanoavb::DescriptorId{{0, DESCRIPTOR_CONTROL, ref.descriptor_index}, 0};
    };
    auto wait_for_set = [&](std::string const& want) {
        for (int i = 0; i < 50 && !contains(fake.sets(), want); ++i) {
            proxy.poll(100, now++);
        }
        proxy.poll(50, now++);
        return contains(fake.sets(), want);
    };
    auto get_bytes = [&](nanoavb::DescriptorId id) {
        std::array<std::uint8_t, 512> out{};
        auto n = proxy.handler().on_get_descriptor_value(AEM_COMMAND_GET_CONTROL, id, {}, out);
        return std::vector<std::uint8_t>(out.begin(), out.begin() + long(n));
    };

    // ---- unreported points serve the authored default ---------------
    auto level = control_of("/input/1/level");
    {
        auto bytes = get_bytes(level);
        EXPECT_EQ(bytes.size(), 4u);
        EXPECT_EQ(float_at(bytes, 0), 0.f);
    }

    // ---- northbound SET_CONTROL -> southbound set -------------------
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, level, float_values({-6.5f})), AEM_STATUS_SUCCESS);
    EXPECT_TRUE(wait_for_set("set input 1 level = -6.5"));
    EXPECT_TRUE(proxy.mirror().value_of(*proxy.ns().resolve_path("/input/1/level")) == "-6.5");
    EXPECT_EQ(proxy.southbound_set_count(), 1u);
    {
        auto bytes = get_bytes(level);
        EXPECT_EQ(bytes.size(), 4u);
        EXPECT_EQ(float_at(bytes, 0), -6.5f);
    }
    // Writing the current value back is echo-suppressed: nothing southbound.
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, level, float_values({-6.5f})), AEM_STATUS_SUCCESS);
    proxy.poll(50, now++);
    EXPECT_EQ(proxy.southbound_set_count(), 1u);
    // Out of the served range is rejected before anything goes southbound.
    EXPECT_EQ(
        proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, level, float_values({50.f})), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(proxy.southbound_set_count(), 1u);

    // ---- booleans, int64 delays, text ------------------------------
    auto mute = control_of("/input/1/mute");
    std::uint8_t on[1] = {255};
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, mute, on), AEM_STATUS_SUCCESS);
    EXPECT_TRUE(wait_for_set("set input 1 mute = true"));
    EXPECT_EQ(get_bytes(mute).at(0), 255u);

    auto delay = control_of("/input/1/delay");
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, delay, int64_values({1000000})), AEM_STATUS_SUCCESS);
    EXPECT_TRUE(wait_for_set("set input 1 delay = 1000000"));
    {
        auto bytes = get_bytes(delay);
        EXPECT_EQ(bytes.size(), 8u);
        std::array<std::uint8_t, 8> b{};
        std::memcpy(b.data(), bytes.data(), 8);
        EXPECT_EQ(detail::decode_linear_field<std::int64_t>(b), std::int64_t(1000000));
    }

    auto name = control_of("/input/1/name");
    {
        auto bytes = get_bytes(name);
        EXPECT_EQ(bytes.size(), 1u);  // unreported: empty, NUL-terminated
    }
    std::string const vocal = "Lead Vocal";
    std::vector<std::uint8_t> text(vocal.begin(), vocal.end());
    text.push_back(0);
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, name, text), AEM_STATUS_SUCCESS);
    EXPECT_TRUE(wait_for_set("set input 1 name = Lead Vocal"));
    {
        auto bytes = get_bytes(name);
        EXPECT_EQ(bytes.size(), vocal.size() + 1);
        EXPECT_TRUE(std::string(bytes.begin(), bytes.end() - 1) == vocal);
    }

    // ---- read-only rejection ----------------------------------------
    auto address = control_of("/input/1/address");
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, address, text), AEM_STATUS_NOT_SUPPORTED);
    auto meter = control_of("/input/2/meter");
    EXPECT_EQ(
        proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, meter, float_values({0.f})), AEM_STATUS_NOT_SUPPORTED);

    // ---- southbound reports -> served GET state ----------------------
    auto const sets_before = proxy.southbound_set_count();
    fake.push("Got: Input 2 Meter = -26.5");
    fake.push("Got: Input 1 Bus 2 Level = -3");
    fake.push("Got (req-7): Input 3 Address = Stage L");
    auto bus2 = proxy.ns().resolve_path("/input/1/bus/2/level");
    for (int i = 0; i < 50 && !proxy.mirror().reported().test(*bus2); ++i) {
        proxy.poll(100, now++);
    }
    proxy.poll(50, now++);
    {
        auto bytes = get_bytes(meter);
        EXPECT_EQ(bytes.size(), 4u);
        EXPECT_EQ(float_at(bytes, 0), -26.5f);
    }
    {
        // The crosspoint row is one array control: [bus 1 default, bus 2].
        auto row = control_of("/input/1/bus/2/level");
        auto bytes = get_bytes(row);
        EXPECT_EQ(bytes.size(), 8u);
        EXPECT_EQ(float_at(bytes, 0), 0.f);
        EXPECT_EQ(float_at(bytes, 1), -3.f);
    }
    {
        auto bytes = get_bytes(control_of("/input/3/address"));
        EXPECT_TRUE(std::string(bytes.begin(), bytes.end() - 1) == "Stage L");
    }
    // Reports never go back southbound.
    EXPECT_EQ(proxy.southbound_set_count(), sets_before);

    // ---- IDENTIFY stays with the built-in ---------------------------
    nanoavb::DescriptorId identify{{0, DESCRIPTOR_CONTROL, 0}, 0};
    EXPECT_EQ(proxy.handler().on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, identify, on), AEM_STATUS_SUCCESS);
    EXPECT_EQ(get_bytes(identify).at(0), 255u);
    EXPECT_EQ(proxy.southbound_set_count(), sets_before);

    // Offline (not started) nothing was notified; the mirror serves GET.
    EXPECT_EQ(proxy.unsolicited_count(), 0u);
    EXPECT_FALSE(proxy.running());
}

//
// Test Runner
//

TEST(nadia_proxy, silent_core_is_detected_and_reconnect_requests_state_again)
{
    // Same incident as the GALAXY proxy's (2026-10-13 repatch): a Core
    // that silently vanishes must be noticed by the pump, the link
    // declared lost, and a reconnect must redo the state request. The
    // ping is a tagged get whose reply is dropped, never re-served.
    FakeNadia fake;
    EXPECT_TRUE(fake.start());

    NadiaProxyConfig config;
    config.nadia_host = "127.0.0.1";
    config.nadia_port = fake.port;
    config.nadia_entity_id = 0x000732fffe959361ull;
    config.shape = small_shape();
    config.southbound_quiet_ms = 1;
    config.southbound_ping_timeout_ms = 1;
    NadiaProxy proxy(config);
    EXPECT_TRUE(proxy.connect_southbound());
    constexpr std::uint64_t MS = 1'000'000;
    std::uint64_t now = 1000 * MS;

    auto const first_get = controlplane_devices::nadia_get(proxy.ns().patterns.front().addresses);
    auto const first_get_line = first_get.substr(0, first_get.size() - 1);
    for (int i = 0; i < 100 && fake.command_count(first_get_line) == 0; ++i) {
        proxy.poll(20, now);
    }
    EXPECT_EQ(fake.command_count(first_get_line), 1u);

    EXPECT_EQ(proxy.poll(0, now), 0);
    now += 2 * MS;
    EXPECT_EQ(proxy.poll(0, now), 0);
    EXPECT_EQ(proxy.client().ping_count(), 1u);
    for (int i = 0; i < 100 && fake.command_count("get liveness = system level") == 0; ++i) {
        usleep(10000);
    }
    EXPECT_EQ(fake.command_count("get liveness = system level"), 1u);

    now += 2 * MS;
    EXPECT_EQ(proxy.poll(0, now), -1);
    EXPECT_FALSE(proxy.client().connected());
    EXPECT_TRUE(proxy.client().error().find("no reply to liveness ping") != std::string::npos);

    EXPECT_TRUE(proxy.connect_southbound());
    for (int i = 0; i < 100 && fake.command_count(first_get_line) < 2; ++i) {
        proxy.poll(20, now);
    }
    EXPECT_EQ(fake.command_count(first_get_line), 2u);
    // The new connection's quiet clock starts at its first poll.
    EXPECT_EQ(proxy.poll(0, now), 0);

    // The ping's reply keeps the link alive and is not a control report.
    now += 2 * MS;
    EXPECT_EQ(proxy.poll(0, now), 0);
    EXPECT_EQ(proxy.client().ping_count(), 2u);
    for (int i = 0; i < 100 && fake.command_count("get liveness = system level") < 2; ++i) {
        usleep(10000);
    }
    auto const reports_before = proxy.mirror().value_report_count();
    fake.push("Got (liveness): System Level = +0.0dB");
    int got = 0;
    for (int i = 0; i < 50 && got == 0; ++i) {
        got = proxy.poll(20, now);
    }
    EXPECT_EQ(got, 1);
    EXPECT_EQ(proxy.mirror().value_report_count(), reports_before);
    EXPECT_FALSE(proxy.client().ping_outstanding());  // the reply settled it
    // Quiet again: the next ping goes out and the link stays up.
    now += 2 * MS;
    EXPECT_EQ(proxy.poll(0, now), 0);
    EXPECT_TRUE(proxy.client().connected());
    EXPECT_EQ(proxy.client().ping_count(), 3u);
}

TEST_MAIN(statusbar_nadia_proxy, nadia_proxy_loopback_test)
