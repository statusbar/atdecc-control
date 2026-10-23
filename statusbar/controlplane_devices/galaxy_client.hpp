// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane_devices/galaxy_ascii.hpp"
#include "statusbar/net/net_socket.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// TCP transport for the GALAXY ASCII protocol: connect (IPv4/IPv6/mDNS via
// getaddrinfo — mslg-gx-816-<serial>.local names resolve through the
// system's mDNS), send command lines, and poll response lines through a
// GalaxyLineBuffer. Blocking with per-call timeouts; one connection per
// client, single-threaded use.
//
// Liveness: a TCP connection to a device that vanished without a FIN or
// RST (power-cycled, repatched onto another segment) stays "established"
// forever if nothing is ever written to it — a 2026-10-13 repatch left both
// proxies mirroring silent sockets for four days. Two defences:
// kernel keepalive on every connection (a backstop), and the application
// level check_liveness() below, driven by the owner's clock, which pings a
// quiet device with a cheap request and declares the link dead when even
// that goes unanswered, so the owner reconnects and re-requests state.

namespace statusbar::controlplane_devices {

class GalaxyClient
{
  public:
    GalaxyClient() = default;

    /// Resolves and connects with a timeout. False on failure (see error()).
    bool connect(std::string_view host, std::uint16_t port = GALAXY_ASCII_PORT, unsigned timeout_ms = 5000);

    bool connected() const noexcept { return fd_.valid(); }
    void close()
    {
        fd_.close();
        reset_liveness();
    }
    std::string const& error() const noexcept { return error_; }

    /// Application-level liveness, driven by the caller's clock: call it
    /// after every poll_lines() with the same now_ns. Nothing is sent while
    /// the device keeps talking. Once the link has been quiet for quiet_ns
    /// the client sends `ping` (a cheap request the device always answers);
    /// if no byte at all arrives within reply_ns after that, the link is
    /// declared dead: error() explains, the socket is closed and -1 is
    /// returned so the owner reconnects. Returns 1 when a ping was just
    /// sent, 0 otherwise. quiet_ns == 0 disables the check.
    int check_liveness(std::uint64_t now_ns, std::uint64_t quiet_ns, std::uint64_t reply_ns, std::string_view ping);

    /// Pings sent over the client's lifetime (all connections).
    std::size_t ping_count() const noexcept { return ping_count_; }
    /// True between a ping and the next received byte.
    bool ping_outstanding() const noexcept { return ping_outstanding_; }

    /// Sends one command (already newline-terminated by the galaxy_*
    /// formatters). False on a write error.
    bool send(std::string_view command);

    /// Reads whatever arrives within timeout_ms and calls
    /// visit(std::string_view line) per complete line. Returns the number
    /// of lines delivered, 0 on timeout, -1 on EOF or error.
    template <typename Visit>
    int poll_lines(unsigned timeout_ms, Visit&& visit)
    {
        char buf[16384];
        auto n = read_some(buf, sizeof buf, timeout_ms);
        if (n <= 0) {
            return int(n);
        }
        int lines = 0;
        buffer_.feed(std::string_view(buf, std::size_t(n)), [&](std::string_view line) {
            ++lines;
            visit(line);
        });
        return lines;
    }

    /// Keeps polling until no line arrives for quiet_ms (or overall_ms
    /// elapses). Returns total lines delivered.
    template <typename Visit>
    int drain(unsigned quiet_ms, unsigned overall_ms, Visit&& visit)
    {
        int total = 0;
        unsigned waited = 0;
        while (waited < overall_ms) {
            int got = poll_lines(quiet_ms, visit);
            if (got < 0) {
                break;
            }
            if (got == 0) {
                break;  // quiet window elapsed with nothing new
            }
            total += got;
            waited += quiet_ms;
        }
        return total;
    }

  private:
    /// Returns bytes read, 0 on timeout, -1 on EOF/error.
    long read_some(char* buf, std::size_t len, unsigned timeout_ms);

    void reset_liveness() noexcept
    {
        have_baseline_ = false;
        rx_since_check_ = false;
        ping_outstanding_ = false;
        last_rx_ns_ = 0;
        ping_sent_ns_ = 0;
    }

    net::FileDescriptor fd_;
    GalaxyLineBuffer buffer_;
    std::string error_;

    // Liveness state (see check_liveness). last_rx_ns_ is the caller's
    // clock at the first check after a receive, so it lags the actual
    // receive by at most one poll interval.
    bool have_baseline_ = false;
    bool rx_since_check_ = false;
    bool ping_outstanding_ = false;
    std::uint64_t last_rx_ns_ = 0;
    std::uint64_t ping_sent_ns_ = 0;
    std::size_t ping_count_ = 0;
};

}  // namespace statusbar::controlplane_devices
