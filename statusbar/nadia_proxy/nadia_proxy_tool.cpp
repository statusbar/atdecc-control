// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The Nadia CP companion proxy entity (docs/PLAN.md §13).
//
//   statusbar-nadia-cp-proxy <nadia-host> <nadia-entity-id> <interface>
//       [--port n] (--shape inputs=N,buses=N,... | --discover [--max n])
//
//   nadia-host       IPv4/IPv6/mDNS of the Nadia (text-command port 28034)
//   nadia-entity-id  the Nadia's entity_id (hex, e.g. 0x000732fffe959361);
//                    the proxy's own entity_id derives from it and
//                    association_id links the two
//   interface        network interface for the ATDECC control plane
//   --shape          the loaded design's channel counts (see
//                    parse_nadia_shape); the AEM model is generated from it
//   --discover       count the channels with read-only probes instead
//                    (--max caps each family's probe range, default 128)
//
// The proxy advertises via ADP, serves the generated AEM model, and
// mirrors between AECP and the Core text-command protocol. Ctrl-C to
// stop.

#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/controlplane_devices/nadia_discover.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nadia_proxy/nadia_proxy.hpp"
#include "statusbar/nanoavb/nanoavb_controller.hpp"
#include "statusbar/net/net_rawnet.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>

using namespace statusbar;
using namespace statusbar::controlplane_devices;
using namespace statusbar::nadia_proxy;

namespace {

std::uint64_t now_ns()
{
    return std::uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// Preflight: is the derived proxy entity_id already advertised by a live
/// entity? Sends a targeted ENTITY_DISCOVER and listens briefly; a dead
/// instance answers nothing, so only a genuinely live holder (another
/// proxy for the same device, or a low-24-bit derivation collision with
/// an unrelated entity) refuses the launch.
bool entity_id_in_use(std::string const& interface_name, std::uint64_t id_bits)
{
    using namespace statusbar::atdecc;
    ieee::Eui64 const id{
        std::uint8_t(id_bits >> 56),
        std::uint8_t(id_bits >> 48),
        std::uint8_t(id_bits >> 40),
        std::uint8_t(id_bits >> 32),
        std::uint8_t(id_bits >> 24),
        std::uint8_t(id_bits >> 16),
        std::uint8_t(id_bits >> 8),
        std::uint8_t(id_bits)};
    constexpr ieee::Eui64 PREFLIGHT_CONTROLLER_ID(0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x09);
    net::RawnetContext context;
    if (!context.open(interface_name, avtp::AVTP_ETHERTYPE, &ATDECC_MULTICAST_MAC)) {
        return false;  // the proxy's own open will report the real error
    }
    nanoavb::NanoAvbAemController controller{PREFLIGHT_CONTROLLER_ID};
    nanoavb::AemControllerEntityCallbacks callbacks;
    callbacks.send_atdecc_multicast = [&](std::span<std::uint8_t const> packet) -> bool {
        return context.send(&ATDECC_MULTICAST_MAC, packet).has_value();
    };
    callbacks.send_atdecc_unicast = [&](ieee::Eui48 const& dest, std::span<std::uint8_t const> packet) -> bool {
        return context.send(&dest, packet).has_value();
    };
    controller.set_callbacks(std::move(callbacks));
    controller.start();
    controller.discover(id);

    std::array<std::uint8_t, 2048> buf{};
    auto const deadline = now_ns() + 2'500'000'000ull;
    while (now_ns() < deadline) {
        ieee::Eui48 src{};
        ieee::Eui48 dst{};
        while (true) {
            auto result = context.recv(&src, &dst, buf);
            if (!result || *result <= 0) {
                break;
            }
            auto payload = std::span<std::uint8_t const>{buf.data(), std::size_t(*result)};
            if (!payload.empty() && payload[0] == avtp::AvtpSubtype::adp && payload.size() >= AdpDu::LENGTH) {
                AdpDu adp{};
                span_load(adp, payload);
                controller.receive_adp(adp, src, std::int64_t(now_ns()));
            }
        }
        controller.tick(std::int64_t(now_ns()));
        if (controller.find_entity(id) != nullptr) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

int usage()
{
    fprintf(
        stderr,
        "usage: statusbar-nadia-cp-proxy <nadia-host> <nadia-entity-id> <interface> "
        "[--port n] (--shape inputs=N,buses=N,outputs=N,listens=N,vgroups=N,bands=N | --discover [--max n])\n"
        "       [--ping-quiet ms] [--ping-timeout ms]   southbound liveness: ping the Core after ms of\n"
        "       silence (default 10000), reconnect when the ping goes unanswered for ms (default 5000);\n"
        "       --ping-quiet 0 disables\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        return usage();
    }
    NadiaProxyConfig config;
    config.nadia_host = argv[1];
    config.nadia_entity_id = std::strtoull(argv[2], nullptr, 16);
    std::string interface_name = argv[3];
    bool have_shape = false;
    bool discover = false;
    unsigned max = 128;
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> char const* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--discover") {
            discover = true;
        } else if (arg == "--port") {
            auto v = next();
            if (!v) {
                return usage();
            }
            config.nadia_port = std::uint16_t(atoi(v));
        } else if (arg == "--max") {
            auto v = next();
            if (!v) {
                return usage();
            }
            max = unsigned(atoi(v));
        } else if (arg == "--ping-quiet") {
            auto v = next();
            if (!v) {
                return usage();
            }
            config.southbound_quiet_ms = unsigned(atoi(v));
        } else if (arg == "--ping-timeout") {
            auto v = next();
            if (!v) {
                return usage();
            }
            config.southbound_ping_timeout_ms = unsigned(atoi(v));
        } else if (arg == "--shape") {
            auto v = next();
            if (!v || !parse_nadia_shape(v, config.shape)) {
                fprintf(stderr, "bad --shape (expected inputs=N,buses=N,outputs=N,listens=N,vgroups=N,bands=N)\n");
                return 2;
            }
            have_shape = true;
        } else {
            return usage();
        }
    }
    if (config.nadia_entity_id == 0) {
        fprintf(stderr, "nadia-entity-id must be the device's non-zero entity_id\n");
        return 2;
    }
    if (have_shape == discover) {
        fprintf(stderr, "exactly one of --shape or --discover is required\n");
        return 2;
    }

    if (discover) {
        // The proxy generates its AEM from the shape at construction, so
        // the probe runs on a throwaway connection first.
        GalaxyClient probe;
        if (!probe.connect(config.nadia_host, config.nadia_port)) {
            fprintf(stderr, "discover connect failed: %s\n", probe.error().c_str());
            return 1;
        }
        config.shape = discover_nadia_shape(probe, max, stderr);
        probe.close();
    }
    if (config.shape.inputs + config.shape.buses + config.shape.outputs + config.shape.listens + config.shape.vgroups == 0) {
        fprintf(stderr, "nadia-cp-proxy: the design has no channels (nothing loaded?) — refusing to start\n");
        return 1;
    }

    auto const derived = derive_proxy_entity_id(config.nadia_entity_id);
    if (entity_id_in_use(interface_name, derived)) {
        fprintf(
            stderr,
            "nadia-cp-proxy: refusing to start — entity_id 0x%016llx is already advertised on %s\n"
            "(another proxy instance for this device, or a low-24-bit entity-id collision; "
            "stop the other instance first)\n",
            (unsigned long long)derived,
            interface_name.c_str());
        return 1;
    }
    NadiaProxy proxy(config);
    auto id = proxy.proxy_entity_id().get();
    fprintf(
        stderr,
        "nadia-cp-proxy: entity_id 0x%016llx (mirroring 0x%016llx at %s), %zu control points in %zu CONTROL descriptors\n",
        (unsigned long long)id,
        (unsigned long long)config.nadia_entity_id,
        config.nadia_host.c_str(),
        proxy.ns().size(),
        proxy.aem().count_of(atdecc::aem::DESCRIPTOR_CONTROL));

    if (!proxy.connect_southbound()) {
        fprintf(stderr, "southbound connect failed: %s\n", proxy.client().error().c_str());
        return 1;
    }
    fprintf(stderr, "southbound connected; state requested\n");

    auto& stop = itc::install_stop_signal();
    net::MessageReactor reactor{stop, net::monotonic_ns, 10};
    if (auto st = proxy.start(reactor, interface_name); !st) {
        fprintf(stderr, "control plane start failed on %s\n", interface_name.c_str());
        return 1;
    }
    fprintf(stderr, "advertising on %s\n", interface_name.c_str());

    // Southbound loss — a closed socket, a write error, or the liveness
    // check giving up on a silent device — is logged once, then the proxy
    // keeps serving the last mirrored state northbound while it retries
    // every 5 s (one progress line every ~10 min) until the device is
    // back, when the full state is re-requested and re-subscribed. No
    // manual restart is needed after a network outage or a device reboot.
    unsigned failed_attempts = 0;
    while (!stop.stop_requested()) {
        (void)reactor.poll_once(10);
        // Southbound drain is non-blocking: the reactor is the only
        // sleeper, so AECP responses go out with no added latency.
        if (proxy.poll(0, now_ns()) >= 0) {
            continue;
        }
        if (failed_attempts == 0) {
            fprintf(stderr, "southbound connection lost: %s — reconnecting\n", proxy.client().error().c_str());
        }
        if (proxy.connect_southbound()) {
            fprintf(stderr, "southbound reconnected after %u failed attempt(s); state requested\n", failed_attempts);
            failed_attempts = 0;
            continue;
        }
        ++failed_attempts;
        if (failed_attempts == 1 || failed_attempts % 120 == 0) {
            fprintf(
                stderr, "reconnect failed (attempt %u): %s; retrying every 5 s\n", failed_attempts, proxy.client().error().c_str());
        }
        for (int i = 0; i < 500 && !stop.stop_requested(); ++i) {
            (void)reactor.poll_once(10);
        }
    }
    (void)proxy.stop();
    fprintf(stderr, "stopped\n");
    return 0;
}
