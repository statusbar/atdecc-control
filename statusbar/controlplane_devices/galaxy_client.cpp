// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane_devices/galaxy_client.hpp"

#include <netdb.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <netinet/tcp.h>
#include <sys/socket.h>

namespace statusbar::controlplane_devices {

namespace {

/// Kernel keepalive, the backstop under check_liveness(): a peer that
/// vanished without a FIN or RST is torn down by the stack after
/// idle + count * interval seconds even if nothing above ever writes.
/// Best effort — a platform without the per-socket knobs keeps its
/// system-wide defaults (typically two hours), which is still better than
/// never.
void enable_keepalive(int fd)
{
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    [[maybe_unused]] int idle_s = 15;
    [[maybe_unused]] int interval_s = 5;
    [[maybe_unused]] int count = 3;
#if defined(TCP_KEEPIDLE)
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof idle_s);
#elif defined(TCP_KEEPALIVE)
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle_s, sizeof idle_s);
#endif
#if defined(TCP_KEEPINTVL)
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_s, sizeof interval_s);
#endif
#if defined(TCP_KEEPCNT)
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
#endif
}

std::string ms_text(std::uint64_t ns)
{
    return std::to_string(ns / 1'000'000u);
}

/// Non-blocking connect with a poll timeout; returns a connected fd or -1.
int connect_with_timeout(addrinfo const* ai, unsigned timeout_ms, std::string& error)
{
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }
    if (!net::set_nonblocking(fd)) {
        error = "set_nonblocking failed";
        ::close(fd);
        return -1;
    }
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
        error = std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (rc != 0) {
        pollfd pfd{fd, POLLOUT, 0};
        rc = ::poll(&pfd, 1, int(timeout_ms));
        if (rc <= 0) {
            error = rc == 0 ? "connect timeout" : std::strerror(errno);
            ::close(fd);
            return -1;
        }
        int so_error = 0;
        socklen_t len = sizeof so_error;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
            error = std::strerror(so_error != 0 ? so_error : errno);
            ::close(fd);
            return -1;
        }
    }
    if (!net::set_blocking(fd)) {
        error = "set_blocking failed";
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

bool GalaxyClient::connect(std::string_view host, std::uint16_t port, unsigned timeout_ms)
{
    close();
    error_.clear();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    std::string host_str(host);
    auto port_str = std::to_string(port);
    int rc = ::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) {
        error_ = ::gai_strerror(rc);
        return false;
    }
    for (addrinfo const* ai = result; ai != nullptr; ai = ai->ai_next) {
        int fd = connect_with_timeout(ai, timeout_ms, error_);
        if (fd >= 0) {
            enable_keepalive(fd);
            fd_ = net::FileDescriptor(fd);
            break;
        }
    }
    ::freeaddrinfo(result);
    if (!fd_.valid()) {
        if (error_.empty()) {
            error_ = "no usable address";
        }
        return false;
    }
    return true;
}

bool GalaxyClient::send(std::string_view command)
{
    if (!fd_.valid()) {
        error_ = "not connected";
        return false;
    }
    std::size_t sent = 0;
    while (sent < command.size()) {
        auto n = ::write(fd_.get(), command.data() + sent, command.size() - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_ = std::strerror(errno);
            return false;
        }
        sent += std::size_t(n);
    }
    return true;
}

long GalaxyClient::read_some(char* buf, std::size_t len, unsigned timeout_ms)
{
    if (!fd_.valid()) {
        error_ = "not connected";
        return -1;
    }
    pollfd pfd{fd_.get(), POLLIN, 0};
    int rc = ::poll(&pfd, 1, int(timeout_ms));
    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        error_ = std::strerror(errno);
        return -1;
    }
    auto n = ::read(fd_.get(), buf, len);
    if (n == 0) {
        error_ = "connection closed";
        return -1;
    }
    if (n < 0) {
        error_ = std::strerror(errno);
        return -1;
    }
    rx_since_check_ = true;
    return n;
}

int GalaxyClient::check_liveness(std::uint64_t now_ns, std::uint64_t quiet_ns, std::uint64_t reply_ns, std::string_view ping)
{
    if (quiet_ns == 0) {
        return 0;
    }
    if (!fd_.valid()) {
        error_ = "not connected";
        return -1;
    }
    if (rx_since_check_ || !have_baseline_) {
        // Anything received answers an outstanding ping too: the device is
        // alive, whatever it said.
        rx_since_check_ = false;
        have_baseline_ = true;
        last_rx_ns_ = now_ns;
        ping_outstanding_ = false;
        return 0;
    }
    if (ping_outstanding_) {
        if (now_ns - ping_sent_ns_ >= reply_ns) {
            error_ = "no reply to liveness ping within " + ms_text(reply_ns) + " ms (nothing received for " +
                ms_text(now_ns - last_rx_ns_) + " ms)";
            close();
            return -1;
        }
        return 0;
    }
    if (now_ns - last_rx_ns_ >= quiet_ns) {
        if (!send(ping)) {
            // A rebooted peer answers the first write with RST: the write
            // error (or the EOF on the next read) is the loss signal.
            close();
            return -1;
        }
        ping_sent_ns_ = now_ns;
        ping_outstanding_ = true;
        ++ping_count_;
        return 1;
    }
    return 0;
}

}  // namespace statusbar::controlplane_devices
