// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

// WidgetBridge (docs/PLAN.md §12): the M8 HTTP+WebSocket control
// surface. One ControllerSimple discovers 1722.1 entities and crawls
// their descriptors; one HttpServer serves the web app and the
// "statusbar.widgets.v1" WS subprotocol; this class is the registry and
// translator between them.
//
// Threading: everything runs on the reactor thread. The tool's main
// loop is `reactor.poll_once(...)` + `bridge.pump(now_ns)`; tests call
// pump() by hand with a fake ControllerService.
//
// Entity lifecycle: ControllerSimple's background crawl caches every
// descriptor the CONFIGURATION advertises. The bridge watches the
// entity directory each pump: a new entity is "pending" until its
// ENTITY + CONFIGURATION + every advertised CONTROL descriptor is
// cached, then its widgets are built (symbols from derive_aem_symbols
// as stable ids), REGISTER_UNSOLICITED_NOTIFICATION is dispatched, and
// the specs are pushed to every connected client. A departed entity is
// announced and dropped.
//
// Outbound WS discipline: every message goes through a per-client
// outbox flushed in order while ws_send() accepts — the refusal-based
// send never silently drops or reorders a replay.

#include "statusbar/atdecc_tools/atdecc_controller_service.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_simple.hpp"
#include "statusbar/http/http_server.hpp"
#include "statusbar/http/http_static.hpp"
#include "statusbar/widget_bridge/widget_controls.hpp"
#include "statusbar/widget_bridge/widget_labels.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace statusbar::widget_bridge {

struct WidgetBridgeConfig
{
    net::SocketAddress bind = net::SocketAddress::ipv4_loopback(0);
    http::HttpLimits limits{};
    http::StaticManifest const* site = nullptr;  ///< borrowed; may be null
    std::string ws_path = "/ws";

    /// Optional per-option label source (docs/PLAN §12.2); borrowed. A
    /// control it enumerates is upgraded to a labeled select.
    WidgetLabelProvider* labels = nullptr;
};

/// One CONTROL or SIGNAL_SELECTOR descriptor projected for the widget
/// layer. A selector widget is always a select: its options index the
/// descriptor's authored source list (labels resolved from the source
/// descriptors' names), its value is the current source's position, and
/// an unauthored selector (no sources) is shown read-only.
struct WidgetSpec
{
    std::uint16_t control_index = 0;  ///< descriptor_index (of its type)
    bool is_selector = false;         ///< SIGNAL_SELECTOR, not CONTROL
    bool is_matrix = false;           ///< MATRIX, not CONTROL
    std::string symbol;               ///< derive_aem_symbols id — stable across renumbering
    std::string name;                 ///< display name: object_name, else localized, else the symbol
    std::string mfr_name;             ///< the fixed localized (manufacturer) name, "" when unauthored
    std::string authored_name;        ///< the blob's authored object_name (what an empty SET_NAME resets to)
    std::string group;                ///< containing CONTROL_BLOCK's name, "" = block-less
    ControlProjection projection;
    std::vector<std::string> value_labels{};           ///< multi-value linear only: per-value localized names, "" where unauthored
    bool refresh_pending = false;                      ///< a client "get" awaits its GET_CONTROL result — answer even if unchanged
    atdecc::aem::DescriptorControl descriptor{};       ///< CONTROL only: parsed copy for the value codecs
    std::vector<atdecc::aem::SignalSource> sources{};  ///< selector only: the authored source list

    // Matrix only: grid shape, cell wire type, and the chunked-read
    // cursor (a full grid exceeds one AECP payload, so it loads as
    // row-sliced GET_MATRIX regions; projection.current is the grid).
    std::uint16_t matrix_width = 0;
    std::uint16_t matrix_height = 0;
    std::uint16_t matrix_value_type = 0;
    std::uint16_t grid_next_row = 0;
    bool grid_loaded = false;
    std::vector<std::string> row_labels{};  ///< provider-supplied source names
};

/// Above this widget count an entity is "jumbo": clients get a group
/// summary instead of every widget spec, and open groups on demand (the
/// nadia-CP proxy serves 55k+ CONTROLs — no browser wants those pushed).
inline constexpr std::size_t JUMBO_WIDGET_THRESHOLD = 512;

/// One AVB_INTERFACE descriptor (IEEE 1722.1 7.2.8) projected for the
/// client: the fields the crawl read (identity, gPTP priorities, clock
/// quality, domain, intervals) and the live GET_AVB_INFO state (7.4.40) —
/// the grandmaster it follows, the measured propagation delay, asCapable
/// and the SRP/gPTP flags — polled every AVB_INFO_POLL_NS and refreshed by
/// the entity's unsolicited GET_AVB_INFO notifications.
struct InterfaceInfo
{
    std::uint16_t index = 0;
    std::string name;
    ieee::Eui48 mac{};
    ieee::Eui64 clock_identity{};
    std::uint16_t interface_flags = 0;
    std::uint8_t priority1 = 0;
    std::uint8_t clock_class = 0;
    std::uint16_t offset_scaled_log_variance = 0;
    std::uint8_t clock_accuracy = 0;
    std::uint8_t priority2 = 0;
    std::uint8_t domain_number = 0;
    std::int8_t log_sync_interval = 0;
    std::int8_t log_announce_interval = 0;
    std::int8_t log_pdelay_interval = 0;
    std::uint16_t port_number = 0;

    struct MsrpMapping
    {
        std::uint8_t traffic_class = 0;
        std::uint8_t priority = 0;
        std::uint16_t vlan_id = 0;
    };

    bool live = false;  ///< a GET_AVB_INFO answer has arrived
    ieee::Eui64 gptp_grandmaster_id{};
    std::uint32_t propagation_delay = 0;
    std::uint8_t gptp_domain_number = 0;
    std::uint8_t avb_flags = 0;  ///< atdecc::aem::avb_info_flags
    std::vector<MsrpMapping> msrp_mappings;
    std::int64_t last_poll_ns = 0;
};

struct EntityWidgets
{
    ieee::Eui64 entity_id{};
    std::uint64_t entity_model_id = 0;
    std::string name;

    // Extra ENTITY-descriptor detail for the client's entity header
    // (docs/PLAN §12.5); all optional — empty/zero fields are omitted.
    std::string vendor;                ///< vendor_name_string, resolved via LOCALE/STRINGS
    std::string model;                 ///< model_name_string, resolved
    std::string firmware;              ///< firmware_version (AtdeccString)
    std::string serial;                ///< serial_number (AtdeccString)
    std::string group;                 ///< group_name (AtdeccString)
    std::uint16_t talker_sources = 0;  ///< talker_stream_sources
    std::uint16_t listener_sinks = 0;  ///< listener_stream_sinks
    std::uint16_t configurations = 0;  ///< configurations_count
    std::uint16_t current_config = 0;  ///< current_configuration

    std::vector<WidgetSpec> widgets;        ///< controls sorted first, then selectors
    std::vector<InterfaceInfo> interfaces;  ///< AVB_INTERFACEs, by descriptor index

    /// The live-value sweep (docs/PLAN §12.2): crawled CONTROL descriptors
    /// carry authored currents, not runtime state, so after unsolicited
    /// registration confirms the bridge GET_CONTROLs every decodable
    /// control. Strictly one in flight per entity — the AEM window is
    /// shared with the crawl, grid reads, and other entities, and a
    /// failed send's completion carries no control index, so serializing
    /// is what makes every control retryable instead of silently skipped.
    std::deque<std::uint16_t> sweep_queue;
    std::int32_t sweep_in_flight = -1;  ///< control index awaited, -1 = none
    int sweep_retries = 0;              ///< consecutive failures for sweep_in_flight

    [[nodiscard]] auto find(std::uint16_t control_index) -> WidgetSpec*;

    [[nodiscard]] auto jumbo() const -> bool { return widgets.size() > JUMBO_WIDGET_THRESHOLD; }
    [[nodiscard]] auto find_selector(std::uint16_t selector_index) -> WidgetSpec*;
    [[nodiscard]] auto find_matrix(std::uint16_t matrix_index) -> WidgetSpec*;
};

class WidgetBridge : public http::WsEndpoint
{
  public:
    WidgetBridge(std::unique_ptr<atdecc_tools::ControllerService> service, WidgetBridgeConfig config);

    [[nodiscard]] auto valid() const -> bool;

    /// The two Pollables to register with the MessageReactor.
    [[nodiscard]] auto http_server() -> http::HttpServer& { return http_; }
    [[nodiscard]] auto controller() -> atdecc_tools::ControllerSimple& { return ctrl_; }

    /// Drive the bridge: drain controller events, reconcile the entity
    /// directory, build newly-complete entities, flush client outboxes.
    /// Call after every reactor poll (and from tests, by hand).
    void pump(std::int64_t now_ns);

    /// Re-read every entity's live values — the control sweep, selector
    /// GETs, and matrix grid loads restart, and changes fan out to the
    /// clients (unchanged values stay quiet). Used after out-of-band
    /// state changes (an offline scene recall).
    void refresh_entity_values(std::int64_t now_ns);

    /// Advertise the offline scene endpoints (docs/PLAN §12.8) in the
    /// hello frame ("scenes": true) so the site shows its scene
    /// capture/recall buttons only where GET/POST /scene/* exist.
    void set_scene_endpoints() { scene_endpoints_ = true; }

    [[nodiscard]] auto entities() const -> std::map<ieee::Eui64, EntityWidgets> const& { return entities_; }
    [[nodiscard]] auto client_count() const -> std::size_t { return clients_.size(); }

    // -- WsEndpoint --
    void on_ws_open(std::size_t slot, http::HttpRequest const& request) override;
    void on_ws_message(std::size_t slot, std::span<std::uint8_t const> payload, bool is_text) override;
    void on_ws_closed(std::size_t slot) override;

  private:
    struct Client
    {
        std::deque<std::string> outbox;
        /// Jumbo entities only: the CONTROL_BLOCK groups this client has
        /// open (entity id -> group names). Widget specs and value pushes
        /// for a jumbo entity reach only clients with the group open.
        std::map<std::uint64_t, std::set<std::string>> open_groups;
    };

    void handle_event(atdecc_tools::ControllerEvent const& event, std::int64_t now_ns);
    /// Where a CONTROL value payload came from — decides whether an
    /// unchanged value still broadcasts (see the definition).
    enum class ValueSource : std::uint8_t
    {
        set_echo,     ///< a SET_CONTROL completion: always broadcasts
        get_result,   ///< a GET_CONTROL completion (sweep or client "get")
        unsolicited,  ///< an entity push
    };
    void handle_control_response(
        ieee::Eui64 const& entity_id, std::uint8_t aem_status, std::span<std::uint8_t const> response, ValueSource source);
    void start_control_sweep(EntityWidgets& entity);
    void pump_control_sweeps(std::int64_t now_ns);
    void handle_selector_response(ieee::Eui64 const& entity_id, std::span<std::uint8_t const> response);
    void handle_matrix_response(ieee::Eui64 const& entity_id, std::span<std::uint8_t const> response, bool from_get);
    void request_matrix_chunk(ieee::Eui64 const& entity_id, WidgetSpec& spec, std::int64_t now_ns);
    void reconcile_entities(std::int64_t now_ns);
    auto try_build_entity(ieee::Eui64 const& entity_id, std::int64_t now_ns) -> bool;
    void apply_labels(std::uint64_t entity_model_id, WidgetSpec& spec);

    void send_to(std::size_t slot, std::string message);
    void broadcast(std::string const& message);
    void flush_outboxes();
    /// Fans a widget-scoped message: to everyone for a regular entity, to
    /// clients with the widget's group open for a jumbo one.
    void broadcast_scoped(ieee::Eui64 const& entity_id, WidgetSpec const& spec, std::string const& message);
    void replay_group_to(std::size_t slot, EntityWidgets const& entity, std::string const& group);
    void handle_client_group(std::size_t slot, ieee::Eui64 const& entity_id, std::string const& group, bool open);
    void replay_entities_to(std::size_t slot);

    [[nodiscard]] auto message_hello() const -> std::string;
    [[nodiscard]] static auto message_entity(EntityWidgets const& e) -> std::string;
    [[nodiscard]] static auto message_widget(EntityWidgets const& e, WidgetSpec const& w) -> std::string;
    [[nodiscard]] auto message_groups(EntityWidgets const& e) const -> std::vector<std::string>;
    [[nodiscard]] static auto message_entity_done(EntityWidgets const& e) -> std::string;
    [[nodiscard]] static auto message_departed(ieee::Eui64 const& entity_id) -> std::string;
    [[nodiscard]] static auto message_value(ieee::Eui64 const& entity_id, WidgetSpec const& w) -> std::string;
    [[nodiscard]] static auto message_matrix_run(
        ieee::Eui64 const& entity_id, WidgetSpec const& w, std::size_t at, std::span<double const> values) -> std::string;
    [[nodiscard]] static auto message_error(std::string_view reason, ieee::Eui64 const& entity_id, std::int64_t control_index)
        -> std::string;

    enum class WidgetAddress : std::uint8_t
    {
        control,
        selector,
        matrix,
    };
    void handle_client_get(
        std::size_t slot, std::uint64_t entity_id, std::int64_t control_index, WidgetAddress address, std::int64_t now_ns);
    void handle_client_set(
        std::size_t slot,
        std::uint64_t entity_id,
        std::int64_t control_index,
        WidgetAddress address,
        std::int64_t cell,
        std::span<double const> values,
        std::int64_t now_ns);
    void handle_client_rename(
        std::size_t slot,
        std::uint64_t entity_id,
        std::int64_t control_index,
        WidgetAddress address,
        std::string_view name,
        std::int64_t now_ns);
    void handle_name_response(ieee::Eui64 const& entity_id, std::span<std::uint8_t const> response);

    // -- AVB interfaces: static detail from the crawl, live GET_AVB_INFO --
    /// How often each interface's GET_AVB_INFO is re-read (an entity that
    /// pushes unsolicited GET_AVB_INFO refreshes sooner).
    static constexpr std::int64_t AVB_INFO_POLL_NS = 5'000'000'000;
    void request_avb_info(ieee::Eui64 const& entity_id, InterfaceInfo& iface, std::int64_t now_ns);
    /// Re-read one AVB_INTERFACE descriptor: a proxy fills its gPTP fields
    /// in moments after it starts advertising (mirrored from its device),
    /// so the crawl's copy can predate them.
    void request_interface_descriptor(ieee::Eui64 const& entity_id, InterfaceInfo const& iface, std::int64_t now_ns);
    void handle_interface_descriptor_response(ieee::Eui64 const& entity_id, std::span<std::uint8_t const> response);
    /// The descriptor's gPTP port fields (everything but the name) into @p iface.
    static void fill_interface_fields(InterfaceInfo& iface, atdecc::aem::DescriptorAvbInterface const& d);
    void pump_avb_info(std::int64_t now_ns);
    void handle_avb_info_response(ieee::Eui64 const& entity_id, std::span<std::uint8_t const> response);
    [[nodiscard]] static auto message_interface(EntityWidgets const& e, InterfaceInfo const& iface) -> std::string;
    [[nodiscard]] static auto message_avb_info(EntityWidgets const& e, InterfaceInfo const& iface) -> std::string;

    atdecc_tools::ControllerSimple ctrl_;
    http::HttpServer http_;
    std::string ws_path_;
    WidgetLabelProvider* labels_ = nullptr;
    /// The largest WS payload the server's per-connection tx buffer can
    /// stage (ws_send refuses a bigger one FOREVER, not just while
    /// draining). Messages are built under it; flush_outboxes drops any
    /// that slipped through rather than wedging the outbox behind them.
    std::size_t max_ws_payload_ = 0;

    std::map<ieee::Eui64, EntityWidgets> entities_;
    std::set<ieee::Eui64> pending_;  ///< discovered, crawl not yet complete
    std::map<std::size_t, Client> clients_;
    bool scene_endpoints_ = false;

    /// Grace period for LOCALE-chased STRINGS reads once every other
    /// descriptor is cached (names prefer localized strings); on expiry
    /// the entity builds anyway with object_name fallbacks.
    static constexpr std::int64_t STRINGS_GRACE_NS = 10'000'000'000;
    std::map<ieee::Eui64, std::int64_t> strings_deadline_;
};

}  // namespace statusbar::widget_bridge
