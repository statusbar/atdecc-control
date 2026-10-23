// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

// Offline scenes (docs/PLAN.md §12.8): the M7 scene machinery wired to
// the offline bridge's in-process model. Two HTTP endpoints on the
// bridge's own server:
//
//   GET  /scene/capture   the current offline state as an M7 scene file
//                         (# statusbar-scene v1, /path<TAB>value) over
//                         every writable AEM-bound point — controls,
//                         matrix crosspoints, and mode selectors;
//   POST /scene/recall    apply a scene file to the offline state
//                         (read-modify-write SETs through the model,
//                         values clamped by the entity like any SET),
//                         then refresh every connected browser. The
//                         response is a plain-text report: matched /
//                         applied / missing / failed.
//
// Galaxy-shaped only: the path<->handle mapping and the handle<->AEM
// binding come from the generated namespace and model, so this attaches
// in --offline-galaxy mode. A captured scene recalls onto the live
// device later through statusbar-galaxy-scene --recall — same format.

#include "statusbar/controlplane/controlplane_aem_binding.hpp"
#include "statusbar/controlplane_devices/galaxy_schema.hpp"
#include "statusbar/http/http_handler.hpp"
#include "statusbar/widget_bridge/offline_service.hpp"
#include "statusbar/widget_bridge/widget_bridge.hpp"

#include <optional>
#include <string>

namespace statusbar::widget_bridge {

class OfflineGalaxyScenes
{
  public:
    /// Borrows everything; register() adds the routes on the bridge's
    /// HTTP server. @p ns and @p binding must be the same generation the
    /// offline blob was built from.
    OfflineGalaxyScenes(
        controlplane_devices::GalaxyNamespace const& ns,
        controlplane::AemBinding const& binding,
        OfflineControllerService& service,
        WidgetBridge& bridge);

    [[nodiscard]] auto register_routes() -> bool;

  private:
    friend class CaptureHandler;
    friend class RecallHandler;

    /// The point's current value in schema units, read from the model
    /// state; nullopt when unreadable (unsupported type).
    [[nodiscard]] auto read_point(controlplane::ControlPointId h) -> std::optional<double>;

    /// Write @p value (schema units) into the model via the proper SET
    /// path for the point's descriptor kind.
    [[nodiscard]] auto write_point(controlplane::ControlPointId h, double value) -> bool;

    [[nodiscard]] auto capture_text() -> std::string;
    [[nodiscard]] auto recall_text(std::string_view text) -> std::string;

    controlplane_devices::GalaxyNamespace const& ns_;
    controlplane::AemBinding const& binding_;
    OfflineControllerService& service_;
    WidgetBridge& bridge_;

    class CaptureHandler;
    class RecallHandler;
    std::unique_ptr<http::HttpHandler> capture_;
    std::unique_ptr<http::HttpHandler> recall_;
};

}  // namespace statusbar::widget_bridge
