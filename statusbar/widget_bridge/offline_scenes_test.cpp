// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Offline scene capture/recall (docs/PLAN.md §12.8) against the real
// generated GX-816 model: GET /scene/capture must serialize the whole
// writable surface in M7 scene format, and POST /scene/recall must apply
// a scene through the model's own SET paths — proven by a re-capture
// reading the recalled value back, and by the report's counters.

#include "statusbar/widget_bridge/offline_scenes.hpp"

#include "statusbar/controlplane_devices/galaxy_schema.hpp"
#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/widget_bridge/offline_service.hpp"
#include "statusbar/widget_bridge/widget_bridge.hpp"

#include <poll.h>
#include <unistd.h>

#include <array>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::widget_bridge;

namespace {

struct SceneHarness
{
    controlplane_devices::GalaxyNamespace ns;
    galaxy_proxy::GalaxyAemModel model;
    OfflineControllerService* service{};
    std::unique_ptr<WidgetBridge> bridge;
    std::unique_ptr<OfflineGalaxyScenes> scenes;
    int64_t now = 1'000'000;

    SceneHarness()
        : ns{controlplane_devices::build_galaxy_816_namespace()}
        , model{galaxy_proxy::build_galaxy_proxy_aem_model(ns, galaxy_proxy::galaxy_model_banks("GX-816"))}
    {
        auto svc = std::make_unique<OfflineControllerService>(model.blob);
        EXPECT_TRUE(svc->valid());
        service = svc.get();
        WidgetBridgeConfig config;
        config.limits.max_connections = 4;
        config.limits.max_body = 1U << 20U;  // a full scene body is ~130 KiB
        bridge = std::make_unique<WidgetBridge>(std::move(svc), config);
        EXPECT_TRUE(bridge->valid());
        scenes = std::make_unique<OfflineGalaxyScenes>(ns, model.binding, *service, *bridge);
        EXPECT_TRUE(scenes->register_routes());
    }

    void turn()
    {
        now += 1'000'000;
        bridge->controller().tick(now);
        struct pollfd pfd{.fd = bridge->http_server().fd(), .events = POLLIN, .revents = 0};
        (void)::poll(&pfd, 1, 5);
        bridge->http_server().on_ready(now);
        bridge->http_server().tick(now);
        bridge->pump(now);
    }

    /// One plain-HTTP request over a fresh connection; the response body
    /// (Content-Length delimited) or "" on any shape failure.
    auto request(std::string const& head_and_body) -> std::string
    {
        auto const a = bridge->http_server().local_addr();
        EXPECT_TRUE(a.has_value());
        if (!a.has_value()) {
            return {};
        }
        int const fd = ::socket(a->family(), SOCK_STREAM, 0);
        if (fd < 0 || ::connect(fd, a->sockaddr(), a->length()) < 0) {
            if (fd >= 0) {
                ::close(fd);
            }
            return {};
        }
        (void)!::send(fd, head_and_body.data(), head_and_body.size(), 0);

        std::string rx;
        std::array<char, 65536> buf{};
        for (int i = 0; i < 400; ++i) {
            turn();
            struct pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, 5) > 0) {
                auto const n = ::recv(fd, buf.data(), buf.size(), 0);
                if (n < 0) {
                    break;
                }
                rx.append(buf.data(), size_t(n));
            }
            auto const head_end = rx.find("\r\n\r\n");
            if (head_end == std::string::npos) {
                continue;
            }
            auto const cl_at = rx.find("Content-Length: ");
            if (cl_at == std::string::npos || cl_at > head_end) {
                break;
            }
            auto const body_len = size_t(std::atoll(rx.c_str() + cl_at + 16));
            if (rx.size() >= head_end + 4 + body_len) {
                ::close(fd);
                return rx.substr(head_end + 4, body_len);
            }
        }
        ::close(fd);
        return {};
    }

    auto get_capture() -> std::string { return request("GET /scene/capture HTTP/1.1\r\nHost: t\r\n\r\n"); }

    auto post_recall(std::string const& scene) -> std::string
    {
        return request(std::format("POST /scene/recall HTTP/1.1\r\nHost: t\r\nContent-Length: {}\r\n\r\n{}", scene.size(), scene));
    }
};

}  // namespace

TEST(offline_scenes, capture_recall_round_trip)
{
    SceneHarness h;

    // Capture: the M7 header, the model line, and a known writable point.
    auto const first = h.get_capture();
    EXPECT_TRUE(first.starts_with("# statusbar-scene v1\n"));
    EXPECT_TRUE(first.find("# model galaxy-816\n") != std::string::npos);
    EXPECT_TRUE(first.find("/processing/input/1/gain\t") != std::string::npos);
    EXPECT_TRUE(first.find("/device/input/1/mode\t") != std::string::npos);

    // Recall a two-line scene: one real point, one unknown path. The
    // unknown path counts as missing; the real one applies.
    auto const report = h.post_recall("# statusbar-scene v1\n"
                                      "# model galaxy-816\n"
                                      "/processing/input/1/gain\t-6.5\n"
                                      "/does/not/exist\t5\n");
    EXPECT_EQ(report, "recall: matched 1 applied 1 read-only 0 failed 0 missing 1\n");

    // The recalled value persisted in the model: a re-capture reads it
    // back through the same GET path every widget uses.
    auto const second = h.get_capture();
    EXPECT_TRUE(second.find("/processing/input/1/gain\t-6.5\n") != std::string::npos);

    // A full round trip: recalling the capture verbatim applies cleanly
    // (every line matches, nothing missing, nothing failed).
    auto const full = h.post_recall(second);
    EXPECT_TRUE(full.starts_with("recall: matched "));
    EXPECT_TRUE(full.find(" failed 0 missing 0\n") != std::string::npos);
}

//
// Test Runner
//

TEST_MAIN(statusbar_widget_bridge, offline_scenes_test)
