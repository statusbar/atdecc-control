// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/controlplane_devices/galaxy_client.hpp"
#include "statusbar/controlplane_devices/nadia_ascii.hpp"
#include "statusbar/controlplane_devices/nadia_mirror.hpp"
#include "statusbar/nadia_proxy/nadia_aem_model.hpp"
#include "statusbar/nadia_proxy/nadia_proxy_handler.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The Nadia CP companion ATDECC entity (docs/PLAN.md §13): advertises its
// own entity_id (derived from the Nadia's, association_id linking the
// two), serves the shape-generated model as its AEM, and mirrors — never
// forwards — between AECP northbound and the Core text-command protocol
// southbound:
//
//   - GET_CONTROL is answered straight from the NadiaMirror (no per-control
//     RAM state in the entity: a full design is ~40k control points, far
//     beyond the storage handler's MAX_CONTROL_STATES), unreported points
//     reading as the authored default.
//   - SET_CONTROL is validated against the served format, translated into
//     Nadia `set` commands (echo-suppressed against the mirrored value, so
//     writing the current value back sends nothing southbound), and
//     answered SUCCESS without storing; the device's own report is what
//     changes the served value.
//   - Southbound reports feed the mirror; changes to bound points fan out
//     as unsolicited SET_CONTROL notifications when the control plane is
//     running (offline, GET already serves the mirror).
//
// IDENTIFY (control 0) stays with the storage handler's built-in.

namespace statusbar::nadia_proxy {

/// proxy entity_id = JDKS OUI-36 bank 0x4 | the Nadia entity_id's low 24
/// bits (stable across restarts — PLAN §7.1 forbids allocating at startup).
constexpr std::uint64_t derive_proxy_entity_id(std::uint64_t nadia_entity_id)
{
    return 0x70B3D5EDC4000000ull | (nadia_entity_id & 0xFFFFFFull);
}

struct NadiaProxyConfig
{
    std::string nadia_host;
    std::uint16_t nadia_port = controlplane_devices::NADIA_ASCII_PORT;
    std::uint64_t nadia_entity_id = 0;       ///< the mirrored device's entity_id
    controlplane_devices::NadiaShape shape;  ///< the loaded design's channel counts
    /// Mirror the NADIA's AVB interface state over IEEE 1722.1 (see
    /// GalaxyProxyConfig::mirror_avb_info).
    bool mirror_avb_info = true;

    /// Southbound liveness (GalaxyClient::check_liveness). After
    /// southbound_quiet_ms without a byte from the Core, poll() sends a
    /// `get` of the system level tagged "liveness" — the Core answers every
    /// get, populated or not, and poll() drops the tagged reply so no
    /// control is re-reported northbound; when nothing at all arrives
    /// within southbound_ping_timeout_ms after that, poll() returns -1 and
    /// the tool reconnects (full state re-request + re-subscribe). 0
    /// disables.
    unsigned southbound_quiet_ms = 10000;
    unsigned southbound_ping_timeout_ms = 5000;
};

class NadiaProxy
{
  public:
    explicit NadiaProxy(NadiaProxyConfig config);

    ieee::Eui64 proxy_entity_id() const noexcept;

    // ---- southbound (Core text commands) -----------------------------

    /// Connects, requests the full state, and subscribes to updates.
    bool connect_southbound();

    /// Drains southbound lines (waiting at most timeout_ms) into the
    /// mirror and fans changed controls out northbound. Also runs the
    /// liveness check (NadiaProxyConfig::southbound_quiet_ms). Returns
    /// lines consumed, -1 on connection loss — including a Core that went
    /// silent — after which connect_southbound() is the recovery.
    int poll(unsigned timeout_ms, std::uint64_t now_ns);

    // ---- northbound (ATDECC control plane) --------------------------

    Status start(net::MessageReactor& reactor, std::string_view interface_name);
    Status stop();
    bool running() const noexcept { return host_.is_running(); }

    // ---- introspection ----------------------------------------------

    controlplane_devices::NadiaNamespace const& ns() const noexcept { return mirror_.ns(); }
    NadiaAemModel const& aem() const noexcept { return aem_; }
    controlplane_devices::NadiaMirror& mirror() noexcept { return mirror_; }
    controlplane_devices::GalaxyClient& client() noexcept { return client_; }
    nanoavb::DescriptorStorageHandler& handler() noexcept { return handler_; }
    avb_entity::AvbEntityHost& host() noexcept { return host_; }

    /// Set commands sent southbound (echo-suppressed sets excluded).
    std::size_t southbound_set_count() const noexcept { return southbound_sets_; }
    /// Unsolicited SET_CONTROL notifications emitted.
    std::size_t unsolicited_count() const noexcept { return unsolicited_; }

    /// The GET_CONTROL value bytes served for a control (test hook).
    std::vector<std::uint8_t> encode_control(std::uint16_t control_index) const;

    /// A northbound SET_CONTROL's value bytes for a control (test hook;
    /// the entity's SET path lands here). Returns the AEM status.
    std::uint8_t on_northbound_control(std::uint16_t control_index, std::span<std::uint8_t const> values);

  private:
    struct Boot;  ///< namespace + AEM model built once, split across members
    static Boot* make_boot(controlplane_devices::NadiaShape const& shape);
    NadiaProxy(NadiaProxyConfig config, Boot boot);

    struct ControlInfo
    {
        std::uint16_t value_type = 0;  ///< masked CONTROL_* family value
        std::uint16_t count = 0;
        std::size_t element_size = 0;
        bool read_only = false;
        bool utf8 = false;
    };

    bool send_southbound_set(controlplane::ControlPointId h, double value);
    bool send_southbound_text(controlplane::ControlPointId h, std::string_view text);
    void notify_control(std::uint16_t control_index);

    NadiaProxyConfig config_;
    NadiaAemModel aem_;
    controlplane_devices::NadiaMirror mirror_;
    controlplane_devices::GalaxyClient client_;
    std::vector<ControlInfo> control_info_;  ///< indexed by CONTROL descriptor_index
    ProxyHandler handler_;                   ///< the host serves descriptors through it: declared first, outlives it
    avb_entity::AvbEntityHost host_;
    std::vector<bool> dirty_controls_;
    std::vector<std::uint16_t> dirty_list_;
    std::size_t southbound_sets_ = 0;
    std::string liveness_ping_;  ///< the tagged get sent to a quiet Core
    std::size_t unsolicited_ = 0;
};

}  // namespace statusbar::nadia_proxy
