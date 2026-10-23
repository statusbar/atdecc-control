// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Live Nadia CP (Meyer LCS Core) state dump and watch tool over the Core
// text-command port — the NadiaMirror counterpart of statusbar-galaxy-mirror.
//
//   statusbar-nadia-mirror <host> [options]
//
//   --shape k=v,...        the loaded design's channel counts:
//                          inputs, buses, outputs, listens, vgroups, bands
//                          (parametric bands per strip); e.g.
//                          --shape inputs=16,buses=8,outputs=16,bands=8
//   --discover             probe the counts from the device instead
//                          (read-only gets of the per-channel "address"
//                          points); prints the shape it found on stderr
//   (default)              get every namespace pattern, print
//                          path<TAB>value for each reported point, then a
//                          reconciliation summary on stderr
//   --watch                subscribe to every namespace pattern and stream
//                          updates until interrupted
//   --get <addresses>      print raw response lines for one get, command
//                          form ("input 1-8 level")
//   --set <path> <value>   set one control point (canonical path), print
//                          the echo
//   --raw <line>           send one arbitrary command line, print the raw
//                          response
//   --port <n>             text-command port (default 28034)
//   --max <n>              --discover probe ceiling per family (default 64)
//
// <host> may be IPv4, IPv6 (link-local with %scope) or a hostname.
// Every value crosses the wire as text over the text-command port.

#include "statusbar/controlplane_devices/galaxy_client.hpp"
#include "statusbar/controlplane_devices/nadia_ascii.hpp"
#include "statusbar/controlplane_devices/nadia_discover.hpp"
#include "statusbar/controlplane_devices/nadia_mirror.hpp"

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
        "usage: statusbar-nadia-mirror <host> [--port n] [--shape inputs=N,buses=N,outputs=N,listens=N,vgroups=N,bands=N | "
        "--discover [--max n]]\n"
        "         [--watch | --get addresses | --set path value | --raw line]\n");
    return 2;
}

void print_raw(std::string_view line)
{
    fwrite(line.data(), 1, line.size(), stdout);
    fputc('\n', stdout);
}

int dump_all(GalaxyClient& client, NadiaMirror& mirror)
{
    for (auto const& pattern : mirror.ns().patterns) {
        if (!client.send(nadia_get(pattern.addresses))) {
            fprintf(stderr, "send failed: %s\n", client.error().c_str());
            return 1;
        }
        client.drain(300, 30000, [&](std::string_view line) { mirror.consume_line(line, now_ns()); });
    }

    auto const& ns = mirror.ns();
    mirror.reported().for_each_set(
        [&](controlplane::ControlPointId h) { printf("%s\t%s\n", ns.adaptor.address_of(h).c_str(), mirror.value_of(h).c_str()); });

    auto missing = mirror.missing();
    fprintf(
        stderr,
        "\nreported %zu of %zu namespace points (%zu populated); %zu missing; %zu unknown addresses; %zu unknown names; %zu noise "
        "lines\n",
        mirror.reported().count(),
        ns.size(),
        mirror.populated().count(),
        missing.count(),
        mirror.unknown_address_count(),
        mirror.unknown_names().size(),
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
    for (auto const& n : mirror.unknown_names()) {
        fprintf(stderr, "unknown: %s\n", n.c_str());
    }
    return 0;
}

int watch(GalaxyClient& client, NadiaMirror& mirror)
{
    for (auto const& pattern : mirror.ns().patterns) {
        if (!client.send(nadia_subscribe(pattern.addresses))) {
            fprintf(stderr, "send failed: %s\n", client.error().c_str());
            return 1;
        }
    }
    fprintf(stderr, "subscribed to %zu patterns (%zu points); ctrl-c to stop\n", mirror.ns().patterns.size(), mirror.ns().size());
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
    std::uint16_t port = NADIA_ASCII_PORT;
    unsigned max = 64;
    bool do_discover = false;
    NadiaShape shape;
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
        } else if (arg == "--max") {
            max = unsigned(atoi(next()));
        } else if (arg == "--shape") {
            if (!parse_nadia_shape(next(), shape)) {
                return usage();
            }
        } else if (arg == "--discover") {
            do_discover = true;
        } else if (arg == "--watch") {
            mode = "watch";
        } else if (arg == "--get" || arg == "--raw") {
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

    if (mode == "dump" || mode == "watch") {
        if (do_discover) {
            shape = discover_nadia_shape(client, max, stderr);
        }
        NadiaMirror mirror(build_nadia_namespace(shape));
        return mode == "dump" ? dump_all(client, mirror) : watch(client, mirror);
    }
    // get / set / raw: send one command, print the raw response.
    std::string command = mode == "get" ? nadia_get(pattern)
        : mode == "set"                 ? nadia_set(nadia_command_address(pattern), set_value)
                                        : pattern + "\n";
    if (!client.send(command)) {
        fprintf(stderr, "send failed: %s\n", client.error().c_str());
        return 1;
    }
    client.drain(1500, 30000, print_raw);
    return 0;
}
