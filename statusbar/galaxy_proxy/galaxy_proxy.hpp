// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/controlplane_devices/galaxy_client.hpp"
#include "statusbar/controlplane_devices/galaxy_mirror.hpp"
#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"
#include "statusbar/galaxy_proxy/galaxy_proxy_handler.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The GALAXY companion ATDECC entity (docs/PLAN.md §7): advertises its own
// entity_id (derived deterministically from the GALAXY's, association_id
// linking the two), serves the M5-generated model as its AEM, and mirrors —
// never forwards — between AECP northbound and the GALAXY ASCII protocol
// southbound:
//
//   - A northbound SET_CONTROL / SET_MATRIX is accepted into the local
//     model (the built-in stores + notifies) and translated into GALAXY
//     set commands, skipping values already at the reported state (which
//     also breaks the set -> echo -> set loop).
//   - Southbound value reports feed the M4 mirror; changes to AEM-bound
//     points re-apply into the entity's control storage and, when the
//     control plane is running, fan out as unsolicited notifications.
//
// v1 limitation (documented, revisit against the live device): the
// 232-crosspoint ceiling is enforced by the GALAXY itself, not
// pre-checked here.
//
// /device/input/N/mode flows both ways as a SIGNAL_SELECTOR whose sources
// are the authored mode triples (galaxy_input_mode_source): a northbound
// SET_SIGNAL_SELECTOR becomes a southbound mode set, and a southbound
// mode report re-applies into the served selector state with unsolicited
// fan-out — the same discipline as CONTROLs.
//
// Matrix crosspoints flow both ways too: a southbound crosspoint report
// re-applies into the served MATRIX grid as a 1x1 SET_MATRIX self-write
// (store + unsolicited fan-out), so controllers see routing changes made
// from Compass live.

namespace statusbar::galaxy_proxy {

/// proxy entity_id = JDKS OUI-36 bank 0x2 | the GALAXY entity_id's low 24
/// bits (stable across restarts — PLAN §7.1 forbids allocating at startup).
constexpr std::uint64_t derive_proxy_entity_id(std::uint64_t galaxy_entity_id)
{
    return 0x70B3D5EDC2000000ull | (galaxy_entity_id & 0xFFFFFFull);
}

struct GalaxyProxyConfig
{
    std::string galaxy_host;
    std::uint16_t galaxy_port = controlplane_devices::GALAXY_ASCII_PORT;
    std::uint64_t galaxy_entity_id = 0;  ///< the mirrored device's entity_id
    unsigned subscribe_throttle_ms = 100;

    /// The mirrored device's /status/model_string; sizes the AEM meter
    /// banks (galaxy_model_banks). Unknown models fall back to GX-816.
    std::string model_string = "GX-816";

    /// Southbound liveness (GalaxyClient::check_liveness). After
    /// southbound_quiet_ms without a byte from the device, poll() sends a
    /// describe of /status/model_string — a request the device always
    /// answers and whose reply the mirror ignores, so no control is
    /// re-reported northbound; when nothing at all arrives within
    /// southbound_ping_timeout_ms after that, poll() returns -1 and the
    /// tool reconnects (full state re-request + re-subscribe) instead of
    /// mirroring a vanished device forever. 0 disables.
    unsigned southbound_quiet_ms = 10000;
    unsigned southbound_ping_timeout_ms = 5000;

    /// Mirror the device's AVB interface state over IEEE 1722.1: the proxy
    /// reads the GALAXY's AVB_INTERFACE descriptors and polls its
    /// GET_AVB_INFO (plus its unsolicited notifications) and serves them as
    /// its own, so controllers see the device's gPTP port, not the proxy
    /// host's. Off => the proxy reports what it observes on the wire.
    bool mirror_avb_info = true;
};

class GalaxyProxy
{
  public:
    explicit GalaxyProxy(GalaxyProxyConfig config);

    ieee::Eui64 proxy_entity_id() const noexcept;

    // ---- southbound (GALAXY ASCII) ----------------------------------

    /// Connects, requests the full state, and subscribes to updates.
    bool connect_southbound();

    /// Drains southbound lines (waiting at most timeout_ms) into the
    /// mirror, then re-applies changed AEM-bound values into the entity's
    /// control storage (with unsolicited fan-out when running). Also runs
    /// the liveness check (GalaxyProxyConfig::southbound_quiet_ms).
    /// Returns lines consumed, -1 on connection loss — including a device
    /// that went silent — after which connect_southbound() is the recovery.
    int poll(unsigned timeout_ms, std::uint64_t now_ns);

    // ---- northbound (ATDECC control plane) --------------------------

    Status start(net::MessageReactor& reactor, std::string_view interface_name);
    Status stop();
    bool running() const noexcept { return host_.is_running(); }

    // ---- introspection ----------------------------------------------

    controlplane_devices::GalaxyNamespace const& ns() const noexcept { return mirror_.ns(); }
    GalaxyAemModel const& aem() const noexcept { return aem_; }
    controlplane_devices::GalaxyMirror& mirror() noexcept { return mirror_; }
    controlplane_devices::GalaxyClient& client() noexcept { return client_; }
    nanoavb::DescriptorStorageHandler& handler() noexcept { return handler_; }
    avb_entity::AvbEntityHost& host() noexcept { return host_; }

    /// Set commands sent southbound (echo-suppressed sets excluded).
    std::size_t southbound_set_count() const noexcept { return southbound_sets_; }

  private:
    struct Boot;  ///< namespace + AEM model built once, split across members
    static Boot* make_boot(std::string_view model_string);
    GalaxyProxy(GalaxyProxyConfig config, Boot boot);

    struct ControlInfo
    {
        std::uint16_t value_type = 0;  ///< masked CONTROL_* family value
        std::uint16_t count = 0;
        std::size_t element_size = 0;
        bool read_only = false;
    };

    std::uint8_t on_northbound_control(std::uint16_t control_index, std::span<std::uint8_t const> values);
    std::uint8_t on_northbound_matrix(std::uint16_t matrix_index, nanoavb::DescriptorStorageHandler::MatrixWrite const& w);
    std::uint8_t on_northbound_selector(std::uint16_t selector_index, atdecc::aem::SignalSource const& source);
    bool send_southbound_set(controlplane::ControlPointId h, double value);
    void apply_southbound_control(std::uint16_t control_index, std::uint64_t now_ns);
    void apply_southbound_selector(std::uint16_t selector_index);
    void apply_southbound_matrix_cell(std::uint16_t matrix_index, std::uint16_t cell);

    GalaxyProxyConfig config_;
    GalaxyAemModel aem_;
    controlplane_devices::GalaxyMirror mirror_;
    controlplane_devices::GalaxyClient client_;
    std::vector<ControlInfo> control_info_;  ///< indexed by CONTROL descriptor_index
    ProxyHandler handler_;                   ///< the host serves descriptors through it: declared first, outlives it
    avb_entity::AvbEntityHost host_;
    std::vector<bool> dirty_controls_;
    std::vector<bool> dirty_selectors_;
    std::vector<std::uint16_t> matrix_value_types_;                            ///< per MATRIX descriptor_index
    std::vector<std::pair<std::uint16_t, std::uint16_t>> dirty_matrix_cells_;  ///< (matrix_index, cell)
    std::size_t southbound_sets_ = 0;
    std::string liveness_ping_;  ///< the describe sent to a quiet device

    /// True while apply_southbound_control stores mirrored device state:
    /// the storage handler runs the same change callback as a controller's
    /// SET, and a self-apply must bypass the northbound checks (read-only
    /// rejection, snapshot recall) or read-only controls never get a
    /// served GET state.
    bool applying_southbound_ = false;
};

}  // namespace statusbar::galaxy_proxy
