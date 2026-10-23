// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Loopback integration test: a minimal fake GALAXY ASCII server (canned
// values from the generated schema, pattern gets, one subscription push)
// exercises GalaxyClient + GalaxyMirror end to end without hardware.

#include "statusbar/controlplane_devices/galaxy_client.hpp"

#include "statusbar/controlplane_devices/galaxy_mirror.hpp"
#include "statusbar/test/test.hpp"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::controlplane_devices;

namespace {

/// Accepts one connection on 127.0.0.1:<ephemeral>, answers a handful of
/// canned requests, then pushes one unsolicited update and closes.
struct FakeGalaxy
{
    int listen_fd = -1;
    std::uint16_t port = 0;
    std::thread thread;

    bool start()
    {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listen_fd, 1) != 0) {
            ::close(listen_fd);
            listen_fd = -1;
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
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        auto reply = [&](std::string_view text) { (void)!::write(fd, text.data(), text.size()); };
        std::string pending;
        char buf[4096];
        bool done = false;
        while (!done) {
            auto n = ::read(fd, buf, sizeof buf);
            if (n <= 0) {
                break;
            }
            pending.append(buf, std::size_t(n));
            std::size_t at;
            while ((at = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, at);
                pending.erase(0, at + 1);
                if (line == "/processing/input/1/gain") {
                    reply("/processing/input/1/gain='-6.5'\r\n");
                } else if (line == "/processing/input/\\d/mute") {
                    reply("/processing/input/1/mute='false'\r\n/processing/input/2/mute='true'\r\n");
                } else if (line == "?/processing/input/1/mute") {
                    reply("?{'/processing/input/1/mute': { 'description' : 'Input 1 Mute'}}\r\n");
                } else if (line.rfind("+/status/meter/input/1 ", 0) == 0) {
                    // Subscription ack-style report now, one push later,
                    // then hang up.
                    reply("/status/meter/input/1='-90'\r\n");
                    reply("/status/meter/input/1='-42.5'\r\n");
                    done = true;
                } else {
                    reply("ERROR: unhandled\r\n");
                }
            }
        }
        ::close(fd);
    }

    ~FakeGalaxy()
    {
        if (thread.joinable()) {
            thread.join();
        }
        if (listen_fd >= 0) {
            ::close(listen_fd);
        }
    }
};

/// A device that accepts connection after connection, records every line
/// it is sent and answers nothing unless the test pushes a line itself —
/// the shape of a GALAXY that is alive but has nothing to report, and (once
/// it stops answering pings) of one that silently went away.
struct SilentDevice
{
    int listen_fd = -1;
    std::uint16_t port = 0;
    std::thread thread;
    std::mutex mutex;
    std::vector<std::string> lines;
    std::atomic<int> conn_fd{-1};
    std::atomic<int> accepted{0};
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
            ++accepted;
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
                    std::lock_guard lock(mutex);
                    lines.push_back(pending.substr(0, at));
                    pending.erase(0, at + 1);
                }
            }
            conn_fd = -1;
            ::close(fd);
        }
    }

    std::size_t count(std::string const& line)
    {
        std::lock_guard lock(mutex);
        std::size_t n = 0;
        for (auto const& l : lines) {
            n += l == line;
        }
        return n;
    }

    void push(std::string const& text)
    {
        int fd = conn_fd;
        if (fd >= 0) {
            (void)!::write(fd, text.data(), text.size());
        }
    }

    ~SilentDevice()
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

constexpr std::uint64_t MS = 1'000'000;

}  // namespace

TEST(galaxy_client, liveness_pings_a_quiet_device_and_gives_up_on_silence)
{
    SilentDevice device;
    EXPECT_TRUE(device.start());
    GalaxyClient client;
    EXPECT_TRUE(client.connect("127.0.0.1", device.port, 2000));
    auto const ping = galaxy_describe("/status/model_string");
    auto const quiet = 10 * MS;
    auto const reply = 5 * MS;

    // First check sets the baseline; nothing is sent while within quiet.
    std::uint64_t now = 100 * MS;
    EXPECT_EQ(client.check_liveness(now, quiet, reply, ping), 0);
    EXPECT_EQ(client.check_liveness(now + 9 * MS, quiet, reply, ping), 0);
    EXPECT_EQ(client.ping_count(), 0u);

    // Quiet elapsed: one ping goes out, and only one.
    EXPECT_EQ(client.check_liveness(now + 10 * MS, quiet, reply, ping), 1);
    EXPECT_TRUE(client.ping_outstanding());
    EXPECT_EQ(client.check_liveness(now + 12 * MS, quiet, reply, ping), 0);
    EXPECT_EQ(client.ping_count(), 1u);
    for (int i = 0; i < 100 && device.count("?/status/model_string") == 0; ++i) {
        usleep(10000);
    }
    EXPECT_EQ(device.count("?/status/model_string"), 1u);

    // Reply window elapsed with silence: dead, closed, explained.
    EXPECT_EQ(client.check_liveness(now + 15 * MS, quiet, reply, ping), -1);
    EXPECT_FALSE(client.connected());
    EXPECT_TRUE(client.error().find("no reply to liveness ping") != std::string::npos);

    // Disabled: never pings, never gives up.
    EXPECT_TRUE(client.connect("127.0.0.1", device.port, 2000));
    EXPECT_EQ(client.check_liveness(now, 0, reply, ping), 0);
    EXPECT_EQ(client.check_liveness(now + 1000 * MS, 0, reply, ping), 0);
    EXPECT_EQ(client.ping_count(), 1u);
}

TEST(galaxy_client, liveness_any_received_byte_answers_the_ping)
{
    SilentDevice device;
    EXPECT_TRUE(device.start());
    GalaxyClient client;
    EXPECT_TRUE(client.connect("127.0.0.1", device.port, 2000));
    auto const ping = galaxy_describe("/status/model_string");
    auto const quiet = 10 * MS;
    auto const reply = 5 * MS;
    int lines = 0;
    auto consume = [&](std::string_view) { ++lines; };

    std::uint64_t now = 100 * MS;
    EXPECT_EQ(client.check_liveness(now, quiet, reply, ping), 0);
    EXPECT_EQ(client.check_liveness(now + 10 * MS, quiet, reply, ping), 1);
    for (int i = 0; i < 100 && device.conn_fd < 0; ++i) {
        usleep(10000);
    }
    // The device answers (with anything): the ping is settled and the
    // quiet clock restarts from that receive.
    device.push("?{'/status/model_string': {}}\r\n");
    for (int i = 0; i < 100 && lines == 0; ++i) {
        (void)client.poll_lines(20, consume);
    }
    EXPECT_EQ(lines, 1);
    EXPECT_EQ(client.check_liveness(now + 12 * MS, quiet, reply, ping), 0);
    EXPECT_FALSE(client.ping_outstanding());
    EXPECT_EQ(client.check_liveness(now + 21 * MS, quiet, reply, ping), 0);
    EXPECT_TRUE(client.connected());
    // ... and a second quiet period earns a second ping.
    EXPECT_EQ(client.check_liveness(now + 22 * MS, quiet, reply, ping), 1);
    EXPECT_EQ(client.ping_count(), 2u);

    // A fresh connection starts over: a baseline, no inherited ping.
    client.close();
    EXPECT_TRUE(client.connect("127.0.0.1", device.port, 2000));
    EXPECT_FALSE(client.ping_outstanding());
    EXPECT_EQ(client.check_liveness(now + 1000 * MS, quiet, reply, ping), 0);
}

TEST(galaxy_client, loopback_mirror_end_to_end)
{
    FakeGalaxy fake;
    EXPECT_TRUE(fake.start());

    GalaxyClient client;
    EXPECT_TRUE(client.connect("127.0.0.1", fake.port, 2000));
    EXPECT_TRUE(client.connected());

    GalaxyMirror mirror;
    std::uint64_t now = 1;
    auto consume = [&](std::string_view line) { mirror.consume_line(line, now++); };

    // Point get.
    EXPECT_TRUE(client.send(galaxy_get("/processing/input/1/gain")));
    // Pattern get (server-side regex).
    EXPECT_TRUE(client.send(galaxy_get("/processing/input/\\d/mute")));
    // Describe.
    EXPECT_TRUE(client.send(galaxy_describe("/processing/input/1/mute")));
    // Subscribe; the fake pushes one update and closes.
    EXPECT_TRUE(client.send(galaxy_subscribe("/status/meter/input/1", 100)));

    client.drain(500, 5000, consume);

    EXPECT_EQ(mirror.value_report_count(), 5u);
    EXPECT_EQ(mirror.describe_count(), 1u);
    EXPECT_EQ(mirror.reported().count(), 4u);

    auto gain = mirror.ns().adaptor.resolve("/processing/input/1/gain");
    EXPECT_TRUE(mirror.value_of(*gain) == "-6.5");
    auto mute2 = mirror.ns().adaptor.resolve("/processing/input/2/mute");
    EXPECT_TRUE(mirror.value_of(*mute2) == "true");
    // The subscription push overwrote the initial report.
    auto meter = mirror.ns().adaptor.resolve("/status/meter/input/1");
    EXPECT_TRUE(mirror.value_of(*meter) == "-42.5");
    EXPECT_EQ(controlplane::value_double(mirror.instance().read(*meter).reported), -42.5);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane_devices, galaxy_client_test)
