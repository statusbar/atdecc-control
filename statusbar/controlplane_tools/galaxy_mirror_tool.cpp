// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Live GALAXY state dump and watch tool (M4 acceptance: dump live values
// to compare against Compass).
//
//   statusbar-galaxy-mirror <host> [options]
//
//   (default)              query every schema category, print path<TAB>value
//                          for each reported point, then a reconciliation
//                          summary (missing / unknown paths) on stderr
//   --watch <pattern>      subscribe to a path or server-side regex and
//                          stream updates until interrupted
//   --describe <pattern>   print the raw `?` describe response
//   --get <pattern>        print raw response lines for one get
//   --set <path> <value>   set one control point, print the echo
//   --port <n>             ASCII port (default 25003)
//   --throttle <ms>        subscription throttle (default 100)
//
// <host> may be IPv4, IPv6, or mDNS (mslg-gx-816-<serial>.local).

#include "statusbar/controlplane_devices/galaxy_client.hpp"
#include "statusbar/controlplane_devices/galaxy_mirror.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace statusbar;
using namespace statusbar::controlplane_devices;

namespace {

std::uint64_t now_ns()
{
    return std::uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

int usage()
{
    fprintf(
        stderr,
        "usage: statusbar-galaxy-mirror <host> [--port n] [--throttle ms]\n"
        "         [--watch pattern | --describe pattern | --get pattern | --set path value]\n");
    return 2;
}

void print_raw(std::string_view line)
{
    fwrite(line.data(), 1, line.size(), stdout);
    fputc('\n', stdout);
}

int dump_all(GalaxyClient& client, GalaxyMirror& mirror)
{
    static char const* CATEGORIES[] = {
        "/processing/.*",
        "/device/.*",
        "/system/.*",
        "/project/.*",
        "/entity/.*",
        "/status/.*",
    };
    for (auto* pattern : CATEGORIES) {
        if (!client.send(galaxy_get(pattern))) {
            fprintf(stderr, "send failed: %s\n", client.error().c_str());
            return 1;
        }
        client.drain(1000, 30000, [&](std::string_view line) { mirror.consume_line(line, now_ns()); });
    }

    auto const& ns = mirror.ns();
    mirror.reported().for_each_set(
        [&](controlplane::ControlPointId h) { printf("%s\t%s\n", ns.adaptor.address_of(h).c_str(), mirror.value_of(h).c_str()); });

    auto missing = mirror.missing();
    fprintf(
        stderr,
        "\nreported %zu of %zu schema points; %zu missing; %zu unknown paths; %zu noise lines\n",
        mirror.reported().count(),
        ns.size(),
        missing.count(),
        mirror.unknown_paths().size(),
        mirror.other_count());
    std::size_t listed = 0;
    missing.for_each_set([&](controlplane::ControlPointId h) {
        if (listed++ < 40) {
            fprintf(stderr, "missing: %s\n", ns.adaptor.address_of(h).c_str());
        }
    });
    if (listed > 40) {
        fprintf(stderr, "missing: ... %zu more\n", listed - 40);
    }
    for (auto const& p : mirror.unknown_paths()) {
        fprintf(stderr, "unknown: %s\n", p.c_str());
    }
    return 0;
}

int watch(GalaxyClient& client, GalaxyMirror& mirror, std::string const& pattern, unsigned throttle_ms)
{
    if (!client.send(galaxy_subscribe(pattern, throttle_ms))) {
        fprintf(stderr, "send failed: %s\n", client.error().c_str());
        return 1;
    }
    fprintf(stderr, "subscribed to %s (throttle %u ms); ctrl-c to stop\n", pattern.c_str(), throttle_ms);
    for (;;) {
        int got = client.poll_lines(1000, [&](std::string_view line) {
            auto h = mirror.consume_line(line, now_ns());
            if (h) {
                printf("%s\t%s\n", mirror.ns().adaptor.address_of(*h).c_str(), mirror.value_of(*h).c_str());
                fflush(stdout);
            }
        });
        if (got < 0) {
            fprintf(stderr, "connection lost: %s\n", client.error().c_str());
            return 1;
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        return usage();
    }
    std::string host = argv[1];
    std::uint16_t port = GALAXY_ASCII_PORT;
    unsigned throttle_ms = 100;
    std::string mode = "dump";
    std::string pattern;
    std::string set_value;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> char const* {
            if (i + 1 >= argc) {
                exit(usage());
            }
            return argv[++i];
        };
        if (arg == "--port") {
            port = std::uint16_t(atoi(next()));
        } else if (arg == "--throttle") {
            throttle_ms = unsigned(atoi(next()));
        } else if (arg == "--watch" || arg == "--describe" || arg == "--get") {
            mode = arg.substr(2);
            pattern = next();
        } else if (arg == "--set") {
            mode = "set";
            pattern = next();
            set_value = next();
        } else {
            return usage();
        }
    }

    GalaxyClient client;
    if (!client.connect(host, port)) {
        fprintf(stderr, "connect to %s:%u failed: %s\n", host.c_str(), port, client.error().c_str());
        return 1;
    }

    GalaxyMirror mirror;
    if (mode == "dump") {
        return dump_all(client, mirror);
    }
    if (mode == "watch") {
        return watch(client, mirror, pattern, throttle_ms);
    }
    // describe / get / set: send one command, print the raw response.
    std::string command = mode == "describe" ? galaxy_describe(pattern)
        : mode == "get"                      ? galaxy_get(pattern)
                                             : galaxy_set(pattern, set_value);
    if (!client.send(command)) {
        fprintf(stderr, "send failed: %s\n", client.error().c_str());
        return 1;
    }
    client.drain(1500, 30000, print_raw);
    return 0;
}
