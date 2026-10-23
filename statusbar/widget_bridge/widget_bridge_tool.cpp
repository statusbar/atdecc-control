// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The M8 widget bridge server (docs/PLAN.md §12).
//
//   statusbar-widget-bridge <interface> [--port n] [--manifest path]
//       [--backend raw|avb] [--controller-id 0x...]
//
//   interface      network interface for the ATDECC control plane
//   --port         HTTP/WS listen port (default 8080, all interfaces)
//   --manifest     static site manifest (docs/PLAN §12.5); without it only
//                  /ws is served
//   --backend      controller backend (default: platform default — the
//                  macOS AVB framework there, raw L2 socket elsewhere)
//   --controller-id  this controller's entity_id (default JDKS bridge id)
//
// Browsers connect to ws://host:port/ws (subprotocol
// "statusbar.widgets.v1"); discovered entities' CONTROL descriptors
// stream out as widget specs, SETs round-trip through AECP, and other
// controllers' changes arrive as unsolicited value pushes. Ctrl-C stops.

#include "statusbar/atdecc_tools/atdecc_controller_service.hpp"
#include "statusbar/controlplane_devices/galaxy_schema.hpp"
#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_aem_model_store.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/widget_bridge/galaxy_widget_labels.hpp"
#include "statusbar/widget_bridge/offline_scenes.hpp"
#include "statusbar/widget_bridge/offline_service.hpp"
#include "statusbar/widget_bridge/widget_bridge.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <string>

using namespace statusbar;
using namespace statusbar::widget_bridge;

namespace {

auto now_ns() -> std::int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Reactor adapter for a Pollable the bridge owns.
class Borrowed final : public net::Pollable
{
  public:
    explicit Borrowed(net::Pollable* target)
        : target_{target}
    {}
    [[nodiscard]] auto fd() const noexcept -> int override { return target_->fd(); }
    void on_ready(std::int64_t now) override { target_->on_ready(now); }
    void tick(std::int64_t now) override { target_->tick(now); }
    [[nodiscard]] auto finished() const noexcept -> bool override { return target_->finished(); }

  private:
    net::Pollable* target_;
};

auto usage() -> int
{
    std::println(
        stderr,
        "usage: statusbar-widget-bridge <interface> [--port n] [--manifest path]\n"
        "                               [--backend raw|avb] [--controller-id 0x...]\n"
        "  offline (no interface, no network):\n"
        "       statusbar-widget-bridge --offline <blob.aem> [--port n] [--manifest path]\n"
        "       statusbar-widget-bridge --offline-store <dir> <fingerprint> ...\n"
        "       statusbar-widget-bridge --offline-galaxy [GX-408|GX-816|GX-816-AES3] ...");
    return 2;
}

auto read_file(std::string const& path) -> std::vector<std::uint8_t>
{
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    std::string interface_name;
    std::uint16_t port = 8080;
    std::string manifest_path;
    std::string backend{atdecc_tools::default_controller_backend()};
    std::uint64_t controller_id_bits = 0x70B3D5EDC2100001ULL;  // JDKS OUI-36, widget-bridge block
    std::string offline_blob_path;
    std::string offline_store_dir;
    std::string offline_store_fingerprint;
    std::string offline_galaxy_model;
    bool offline_galaxy = false;

    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        auto next = [&]() -> char const* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (arg == "--port") {
            auto const* v = next();
            if (v == nullptr) {
                return usage();
            }
            port = std::uint16_t(std::strtoul(v, nullptr, 10));
        } else if (arg == "--manifest") {
            auto const* v = next();
            if (v == nullptr) {
                return usage();
            }
            manifest_path = v;
        } else if (arg == "--backend") {
            auto const* v = next();
            if (v == nullptr) {
                return usage();
            }
            backend = v;
        } else if (arg == "--controller-id") {
            auto const* v = next();
            if (v == nullptr) {
                return usage();
            }
            controller_id_bits = std::strtoull(v, nullptr, 16);
        } else if (arg == "--offline") {
            auto const* v = next();
            if (v == nullptr) {
                return usage();
            }
            offline_blob_path = v;
        } else if (arg == "--offline-store") {
            auto const* dir = next();
            auto const* fp = next();
            if (dir == nullptr || fp == nullptr) {
                return usage();
            }
            offline_store_dir = dir;
            offline_store_fingerprint = fp;
        } else if (arg == "--offline-galaxy") {
            offline_galaxy = true;
            if (i + 1 < argc && !std::string_view{argv[i + 1]}.starts_with("--")) {
                offline_galaxy_model = argv[++i];
            }
        } else if (interface_name.empty() && !arg.starts_with("--")) {
            interface_name = arg;
        } else {
            return usage();
        }
    }
    bool const offline = !offline_blob_path.empty() || !offline_store_dir.empty() || offline_galaxy;
    if (interface_name.empty() && !offline) {
        return usage();
    }

    static std::optional<http::StaticManifest> manifest;
    if (!manifest_path.empty()) {
        auto loaded = http::StaticManifest::load(manifest_path);
        if (!loaded) {
            std::println(stderr, "Error: manifest '{}' failed to load", manifest_path);
            return 1;
        }
        manifest.emplace(std::move(*loaded));
        std::println(stderr, "Serving {} static route(s) from {}", manifest->size(), manifest_path);
    }

    // Kept alive for the scene endpoints (--offline-galaxy only).
    static std::optional<controlplane_devices::GalaxyNamespace> galaxy_ns;
    static std::optional<galaxy_proxy::GalaxyAemModel> galaxy_model;
    OfflineControllerService* offline_ptr = nullptr;

    std::unique_ptr<atdecc_tools::ControllerService> service;
    if (offline) {
        std::vector<std::uint8_t> blob;
        if (offline_galaxy) {
            galaxy_ns.emplace(controlplane_devices::build_galaxy_816_namespace());
            galaxy_model.emplace(galaxy_proxy::build_galaxy_proxy_aem_model(
                *galaxy_ns, galaxy_proxy::galaxy_model_banks(offline_galaxy_model.empty() ? "GX-816" : offline_galaxy_model)));
            blob = galaxy_model->blob;
        } else if (!offline_store_dir.empty()) {
            nanoavb::AemModelStore store{offline_store_dir};
            auto loaded = store.load_blob(offline_store_fingerprint);
            if (!loaded.has_value()) {
                std::println(stderr, "Error: fingerprint '{}' not in store '{}'", offline_store_fingerprint, offline_store_dir);
                return 1;
            }
            blob = std::move(*loaded);
        } else {
            blob = read_file(offline_blob_path);
            if (blob.empty()) {
                std::println(stderr, "Error: cannot read blob '{}'", offline_blob_path);
                return 1;
            }
        }
        auto offline_service = std::make_unique<OfflineControllerService>(std::move(blob));
        if (!offline_service->valid()) {
            std::println(stderr, "Error: not a valid .aem descriptor image");
            return 1;
        }
        offline_ptr = offline_service.get();
        service = std::move(offline_service);
        std::println(stderr, "widget bridge OFFLINE: serving the model in-process, no network");
    } else {
        ieee::Eui64 controller_id{};
        controller_id.from_uint64(controller_id_bits);
        std::string backend_error;
        service = atdecc_tools::make_controller_service(backend, interface_name, controller_id, backend_error);
        if (service == nullptr) {
            std::println(stderr, "Error: {}", backend_error);
            return 1;
        }
    }

    // GALAXY proxy models get their schema enum labels (a non-galaxy
    // entity_model_id costs one map miss).
    static GalaxyWidgetLabels galaxy_labels;

    WidgetBridgeConfig config;
    config.bind = net::SocketAddress::ipv4_any(port);
    config.limits.max_connections = 16;
    config.limits.max_body = 1U << 20U;  // a full-scene POST body is ~100 KiB
    config.site = manifest ? &*manifest : nullptr;
    config.labels = &galaxy_labels;
    WidgetBridge bridge{std::move(service), config};
    if (!bridge.valid()) {
        std::println(stderr, "Error: cannot bind HTTP server on port {}", port);
        return 1;
    }

    // Scene capture/recall against the offline state (docs/PLAN §12.8).
    std::optional<OfflineGalaxyScenes> scenes;
    if (offline_ptr != nullptr && galaxy_ns.has_value()) {
        scenes.emplace(*galaxy_ns, galaxy_model->binding, *offline_ptr, bridge);
        if (!scenes->register_routes()) {
            std::println(stderr, "Error: cannot register /scene routes");
            return 1;
        }
        std::println(stderr, "scene endpoints: GET /scene/capture  POST /scene/recall");
    }
    std::println(
        stderr, "widget bridge on {}: http://0.0.0.0:{}/  ws://0.0.0.0:{}/ws", offline ? "(offline)" : interface_name, port, port);

    auto& stop = statusbar::itc::install_stop_signal();
    net::MessageReactor reactor{stop, now_ns, 10};
    // The reactor tolerates fd() == -1 (framework backends deliver via
    // their own marshalling; tick still runs) — add both unconditionally.
    reactor.add(std::make_unique<Borrowed>(&bridge.http_server()));
    reactor.add(std::make_unique<Borrowed>(&bridge.controller()));

    while (!stop.stop_requested()) {
        reactor.poll_once(50);
        bridge.pump(now_ns());
    }
    std::println(stderr, "widget bridge stopped");
    return 0;
}
