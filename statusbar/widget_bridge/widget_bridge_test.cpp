// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The M8 loopback acceptance test (docs/PLAN.md §12): the bridge runs on
// a FAKE ControllerService serving a synthetic 3-control entity, and a
// real WebSocket client on the loopback HTTP server sees the whole
// story — widget specs out, a SET round-tripped through the (fake) AEM
// layer, an unsolicited change pushed live, and the departure. No AVB
// network, no hardware.

#include "statusbar/widget_bridge/widget_bridge.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_control_accessors.hpp"
#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/widget_bridge/offline_service.hpp"
#include "statusbar/widget_bridge/widget_json.hpp"

#include <poll.h>
#include <unistd.h>

#include <array>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::atdecc_tools;
using namespace statusbar::widget_bridge;
using ieee::Eui64;

namespace {

Eui64 const DEVICE{0x70, 0xB3, 0xD5, 0xED, 0xC2, 0x00, 0x00, 0x42};

// ---- the synthetic entity model --------------------------------------------

auto entity_descriptor_bytes() -> std::vector<uint8_t>
{
    DescriptorEntity d{};
    d.entity_id = DEVICE;
    d.entity_model_id.from_uint64(0x70B3D5EDC2AA0001ULL);
    d.entity_name.assign("Widget Test Box");
    d.firmware_version.assign("1.2.3");
    d.serial_number.assign("SN-4242");
    d.group_name.assign("FOH");
    d.talker_stream_sources = 4;
    d.listener_stream_sinks = 2;
    d.configurations_count = 1;
    d.current_configuration = 0;
    d.vendor_name_string = 3;  // STRINGS 0, offset 3 (see strings_descriptor_bytes)
    auto const bytes = make_const_span(d).first(DescriptorEntity::LENGTH);
    return {bytes.begin(), bytes.end()};
}

auto configuration_descriptor_bytes(uint16_t controls = 8, uint16_t blocks = 1) -> std::vector<uint8_t>
{
    DescriptorConfiguration d{};
    d.descriptor_counts[0].descriptor_type = DESCRIPTOR_CONTROL;
    d.descriptor_counts[0].count = controls;
    d.descriptor_counts[1].descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR;
    d.descriptor_counts[1].count = 1;
    d.descriptor_counts[2].descriptor_type = DESCRIPTOR_MATRIX;
    d.descriptor_counts[2].count = 1;
    d.descriptor_counts[3].descriptor_type = DESCRIPTOR_CONTROL_BLOCK;
    d.descriptor_counts[3].count = blocks;
    // LOCALE is counted; the STRINGS behind it deliberately is NOT — the
    // crawl must chase it through the LOCALE's base_strings, like the
    // real GALAXY and JOYNED devices require.
    d.descriptor_counts[4].descriptor_type = DESCRIPTOR_LOCALE;
    d.descriptor_counts[4].count = 1;
    d.descriptor_counts[5].descriptor_type = DESCRIPTOR_AVB_INTERFACE;
    d.descriptor_counts[5].count = 1;
    d.descriptor_counts_count = 6;
    auto const bytes = make_const_span(d).first(DescriptorConfiguration::LENGTH + (6 * sizeof(DescriptorCountEntry)));
    return {bytes.begin(), bytes.end()};
}

/// AVB_INTERFACE 0: the box's one NIC with a gPTP port in domain 0.
auto avb_interface_descriptor_bytes() -> std::vector<uint8_t>
{
    DescriptorAvbInterface d{};
    d.object_name.assign("eth0");
    d.mac_address = Eui48{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    d.clock_identity = Eui64{0x02, 0x11, 0x22, 0xFF, 0xFE, 0x33, 0x44, 0x55};
    d.priority1 = 248;
    d.priority2 = 200;
    d.clock_class = 248;
    d.clock_accuracy = 0xFE;
    d.offset_scaled_log_variance = 0x4100;
    d.domain_number = 0;
    d.log_sync_interval = static_cast<uint8_t>(static_cast<int8_t>(-3));
    d.log_announce_interval = 0;
    d.log_pdelay_interval = 0;
    d.port_number = 1;
    auto const bytes = make_const_span(d).first(DescriptorAvbInterface::LENGTH);
    return {bytes.begin(), bytes.end()};
}

/// CONTROL 0: gain slider (linear float, -90..10 dB, current -12).
/// CONTROL 1: mode select (selector uint16, options {0, 1, 2}, current 1).
/// CONTROL 2: enable toggle (linear uint8, 0..1, current 0).
/// CONTROL 3: mute toggle (linear uint8, AEM 0/255 convention, current 255).
/// CONTROL 4: delay-type select (discrete linear uint8, 0..7 step 1, current 2).
/// CONTROL 5: level meter (array float, -90..20 dB, currents {-26.5, -40}).
/// CONTROL 6: the standard IDENTIFY control (Milan-mandated): momentary
///            0/255, object_name "Blink" but localized string "Identify"
///            — the localized name must win.
/// CONTROL 7: a Meyer-style multi-value telemetry control: 3 x UINT16
///            read-only, multiplier -2, unnamed object with localized
///            "Temperature" and per-value strings NVME / CPU / Case.
auto control_descriptor(uint16_t index) -> DescriptorControl
{
    DescriptorControl d{};
    d.descriptor_index = index;
    if (index == 7) {
        d.localized_description = 4;  // STRINGS 0, offset 4
        std::array<LinearValueEntry<uint16_t>, 3> entries{};
        uint16_t const currents[3] = {4584, 7200, 4700};
        for (size_t i = 0; i < entries.size(); ++i) {
            entries[i].set_minimum(0);
            entries[i].set_maximum(65535);
            entries[i].set_step(1);
            entries[i].set_default_value(29315);
            entries[i].set_current(currents[i]);
            entries[i].unit = ControlUnits{-2, 0};
            entries[i].localized_string = uint16_t(5 + i);  // STRINGS 0, offsets 5..7
        }
        EXPECT_TRUE(init_linear<uint16_t>(d, std::span{entries}, /*read_only=*/true));
        return d;
    }
    if (index == 6) {
        d.object_name.assign("Blink");
        d.localized_description = 2;  // STRINGS 0, offset 2
        d.control_type = CONTROL_TYPE_IDENTIFY;
        LinearValueEntry<uint8_t> e{};
        e.set_minimum(0);
        e.set_maximum(255);
        e.set_step(255);
        e.set_default_value(0);
        e.set_current(0);
        EXPECT_TRUE(init_linear<uint8_t>(d, std::span{&e, 1}));
        return d;
    }
    if (index == 3) {
        d.object_name.assign("Mute");
        d.control_type = CONTROL_TYPE_MUTE;
        LinearValueEntry<uint8_t> e{};
        e.set_minimum(0);
        e.set_maximum(255);
        e.set_step(255);
        e.set_default_value(0);
        e.set_current(255);
        EXPECT_TRUE(init_linear<uint8_t>(d, std::span{&e, 1}));
        return d;
    }
    if (index == 5) {
        d.object_name.assign("Levels");
        ArrayView<float> view{};
        view.minimum = -90.0F;
        view.maximum = 20.0F;
        view.step = 0.5F;
        view.default_value = -90.0F;
        view.unit.code = 0xB0;  // UNIT_CODE_DB
        view.current = {-26.5F, -40.0F};
        EXPECT_TRUE(init_array<float>(d, view, /*read_only=*/true));
        return d;
    }
    if (index == 4) {
        d.object_name.assign("Delay Type");
        LinearValueEntry<uint8_t> e{};
        e.set_minimum(0);
        e.set_maximum(7);
        e.set_step(1);
        e.set_default_value(0);
        e.set_current(2);
        EXPECT_TRUE(init_linear<uint8_t>(d, std::span{&e, 1}));
        return d;
    }
    if (index == 0) {
        d.object_name.assign("Gain");
        d.control_type = CONTROL_TYPE_GAIN;
        LinearValueEntry<float> e{};
        e.set_minimum(-90.0F);
        e.set_maximum(10.0F);
        e.set_step(0.5F);
        e.set_default_value(0.0F);
        e.set_current(-12.0F);
        e.unit.code = 8;  // dB (UNIT_DB code within the AEM units table)
        EXPECT_TRUE(init_linear<float>(d, std::span{&e, 1}));
    } else if (index == 1) {
        d.object_name.assign("Mode");
        SelectorView<uint16_t> view{};
        view.current = 1;
        view.default_value = 0;
        view.options = {0, 1, 2};
        EXPECT_TRUE(init_selector<uint16_t>(d, view));
    } else {
        d.object_name.assign("Enable");
        LinearValueEntry<uint8_t> e{};
        e.set_minimum(0);
        e.set_maximum(1);
        e.set_step(1);
        e.set_default_value(0);
        e.set_current(0);
        EXPECT_TRUE(init_linear<uint8_t>(d, std::span{&e, 1}));
    }
    return d;
}

auto control_descriptor_bytes(uint16_t index) -> std::vector<uint8_t>
{
    auto const d = control_descriptor(index);
    auto const bytes = make_const_span(d).first(d.wire_size());
    return {bytes.begin(), bytes.end()};
}

/// LOCALE "en" advertising one STRINGS descriptor (not in the
/// configuration counts) whose offset 2 carries "Identify".
auto locale_descriptor_bytes() -> std::vector<uint8_t>
{
    DescriptorLocale d{};
    d.locale_identifier.assign("en");
    d.number_of_strings = 1;
    d.base_strings = 0;
    auto const bytes = make_const_span(d).first(DescriptorLocale::LENGTH);
    return {bytes.begin(), bytes.end()};
}

auto strings_descriptor_bytes() -> std::vector<uint8_t>
{
    DescriptorStrings d{};
    d.string_2.assign("Identify");
    d.string_3.assign("Statusbar Test");
    d.string_4.assign("Temperature");
    d.string_5.assign("NVME");
    d.string_6.assign("CPU");
    // offset 7 ("Case") is the last slot; string_7 is left unauthored to
    // prove a blank per-value label ships as "" rather than dropping the array.
    auto const bytes = make_const_span(d).first(DescriptorStrings::LENGTH);
    return {bytes.begin(), bytes.end()};
}

auto selector_source(uint16_t index) -> SignalSource
{
    SignalSource s{};
    s.signal_type = DESCRIPTOR_JACK_INPUT;
    s.signal_index = index;
    s.signal_output = 0;
    return s;
}

/// SIGNAL_SELECTOR 0: "Input Source", two authored JACK_INPUT sources,
/// current = the second.
auto selector_descriptor_bytes() -> std::vector<uint8_t>
{
    DescriptorSignalSelector d{};
    d.descriptor_index = 0;
    d.object_name.assign("Input Source");
    d.sources_offset = uint16_t(DescriptorSignalSelector::LENGTH);
    d.number_of_sources = 2;
    d.current_signal = selector_source(1);
    d.default_signal = selector_source(0);
    auto const head = make_const_span(d).first(DescriptorSignalSelector::LENGTH);
    std::vector<uint8_t> out{head.begin(), head.end()};
    for (uint16_t i = 0; i < 2; ++i) {
        auto const source = selector_source(i);
        auto const bytes = make_const_span(source);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

/// MATRIX 0: "Routing", 4 wide x 3 high, float cells -90..10 dB.
auto matrix_descriptor_bytes() -> std::vector<uint8_t>
{
    DescriptorMatrix d{};
    d.descriptor_index = 0;
    d.object_name.assign("Routing");
    d.control_value_type = pack_control_value_type(false, false, CONTROL_LINEAR_FLOAT);
    d.width = 4;
    d.height = 3;
    d.values_offset = uint16_t(DescriptorMatrix::LENGTH);
    d.number_of_values = 1;
    auto const head = make_const_span(d).first(DescriptorMatrix::LENGTH);
    std::vector<uint8_t> out{head.begin(), head.end()};
    LinearValueEntry<float> entry{};
    entry.set_minimum(-90.0F);
    entry.set_maximum(10.0F);
    entry.set_step(0.5F);
    entry.set_default_value(-90.0F);
    entry.set_current(-90.0F);
    entry.unit.code = 0xB0;  // dB
    auto const trailer = make_const_span(entry);
    out.insert(out.end(), trailer.begin(), trailer.end());
    return out;
}

/// A GET/SET_MATRIX response body: descriptor header + region echo + cells.
auto matrix_response(uint16_t column, uint16_t row, uint16_t width, uint16_t height, std::vector<float> const& cells)
    -> std::vector<uint8_t>
{
    std::vector<uint8_t> out;
    auto push16 = [&](uint16_t v) {
        out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v & 0xFF));
    };
    push16(DESCRIPTOR_MATRIX);
    push16(0);  // descriptor_index
    push16(column);
    push16(row);
    push16(width);
    push16(height);
    push16(uint16_t(cells.size()));  // rep off, horizontal, count
    push16(0);                       // item_offset
    for (auto const v : cells) {
        std::array<uint8_t, 4> b{};
        detail::encode_linear_field(v, b);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

/// CONTROL_BLOCK 0: "Input 1" spanning controls 0..4 (the meter at 5
/// stays block-less). Blocks past 0 (jumbo fixtures) are "Blk <k>",
/// one control each, covering control 7+k.
auto control_block_descriptor_bytes(uint16_t index = 0) -> std::vector<uint8_t>
{
    DescriptorControlBlock d{};
    d.descriptor_index = index;
    if (index == 0) {
        d.object_name.assign("Input 1");
        d.number_of_controls = 5;
        d.base_control = 0;
        d.final_control_index = 4;
    } else {
        d.object_name.assign(("Blk " + std::to_string(index)).c_str());
        d.number_of_controls = 1;
        d.base_control = uint16_t(7 + index);
        d.final_control_index = uint16_t(7 + index);
    }
    auto const bytes = make_const_span(d).first(DescriptorControlBlock::LENGTH);
    return {bytes.begin(), bytes.end()};
}

// ---- the fake backend -------------------------------------------------------

/// No transport: records commands, answers READ_DESCRIPTOR from the
/// synthetic model when the test pumps it, exposes the sink.
class FakeService final : public ControllerService
{
  public:
    struct Sent
    {
        uint16_t command{0};
        Eui64 target{};
        std::vector<uint8_t> payload;
        atdecc::AemCommandCompletion completion{};
        bool completed{false};
    };

    void set_sink(ControllerServiceSink sink) override { sink_ = std::move(sink); }
    void start() override {}
    void tick(int64_t) override {}
    [[nodiscard]] auto pollable() noexcept -> net::Pollable* override { return nullptr; }
    void discover_all() override {}
    [[nodiscard]] auto find_entity(Eui64 const& id) const -> DiscoveredEntity const* override
    {
        for (auto const& e : entities_) {
            if (e.adpdu.entity_id == id) {
                return &e;
            }
        }
        return nullptr;
    }

    auto send_aem_command(Eui64 const& target, uint16_t code, std::span<uint8_t const> payload, atdecc::AemCommandCompletion c)
        -> bool override
    {
        sent_.push_back({code, target, {payload.begin(), payload.end()}, std::move(c), false});
        return true;
    }
    auto register_unsolicited(Eui64 const& target, atdecc::AemCommandCompletion c) -> bool override
    {
        sent_.push_back({AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, target, {}, std::move(c), false});
        return true;
    }
    [[nodiscard]] auto aem_inflight_count() const -> size_t override { return 0; }

    auto read_descriptor(Eui64 const& target, uint16_t type, uint16_t index) -> bool override
    {
        pending_reads_.push_back({target, type, index});
        return true;
    }

    auto set_identify(Eui64 const&, bool, atdecc::AemCommandCompletion) -> bool override { return true; }
    auto get_counters(Eui64 const&, uint16_t, uint16_t) -> bool override { return true; }
    auto set_stream_format(Eui64 const&, uint16_t, uint16_t, uint64_t) -> bool override { return true; }
    auto start_streaming(Eui64 const&, uint16_t, uint16_t) -> bool override { return true; }
    auto stop_streaming(Eui64 const&, uint16_t, uint16_t) -> bool override { return true; }
    auto set_clock_source(Eui64 const&, uint16_t, uint16_t, atdecc::AemCommandCompletion) -> bool override { return true; }
    auto get_clock_source(Eui64 const&, uint16_t, atdecc::AemCommandCompletion) -> bool override { return true; }
    auto set_signal_selector(
        Eui64 const& target,
        uint16_t desc_index,
        uint16_t signal_type,
        uint16_t signal_index,
        uint16_t signal_output,
        atdecc::AemCommandCompletion c) -> bool override
    {
        // Record the triple in the payload slot for byte-level assertions.
        std::vector<uint8_t> triple(6, 0);
        triple[0] = uint8_t(signal_type >> 8);
        triple[1] = uint8_t(signal_type);
        triple[2] = uint8_t(signal_index >> 8);
        triple[3] = uint8_t(signal_index);
        triple[4] = uint8_t(signal_output >> 8);
        triple[5] = uint8_t(signal_output);
        sent_.push_back({AEM_COMMAND_SET_SIGNAL_SELECTOR, target, std::move(triple), std::move(c), false});
        (void)desc_index;
        return true;
    }
    auto get_signal_selector(Eui64 const& target, uint16_t, atdecc::AemCommandCompletion c) -> bool override
    {
        sent_.push_back({AEM_COMMAND_GET_SIGNAL_SELECTOR, target, {}, std::move(c), false});
        return true;
    }
    auto connect_stream(Eui64 const&, uint16_t, Eui64 const&, uint16_t) -> bool override { return true; }
    auto disconnect_stream(Eui64 const&, uint16_t, Eui64 const&, uint16_t) -> bool override { return true; }
    auto connect_tx_stream(Eui64 const&, uint16_t, Eui64 const&, uint16_t) -> bool override { return true; }
    auto disconnect_tx_stream(Eui64 const&, uint16_t, Eui64 const&, uint16_t) -> bool override { return true; }
    auto get_rx_state(Eui64 const&, uint16_t) -> bool override { return true; }
    auto get_tx_state(Eui64 const&, uint16_t) -> bool override { return true; }

    // -- test-side controls --

    /// CONTROLs the configuration advertises; > JUMBO_WIDGET_THRESHOLD
    /// flips the entity into the grouped presentation.
    uint16_t control_count = 8;
    /// CONTROL_BLOCKs advertised (jumbo fixtures raise it so the group
    /// summary spans several chunked frames).
    uint16_t control_block_count = 1;

    void add_entity(Eui64 const& id)
    {
        DiscoveredEntity e{};
        e.adpdu.entity_id = id;
        e.valid = true;
        entities_.push_back(e);
        if (sink_.on_entity_added) {
            sink_.on_entity_added(entities_.back());
        }
    }

    void remove_entity(Eui64 const& id)
    {
        std::erase_if(entities_, [&](DiscoveredEntity const& e) { return e.adpdu.entity_id == id; });
        if (sink_.on_entity_departed) {
            sink_.on_entity_departed(id);
        }
    }

    /// Answer every pending READ_DESCRIPTOR from the synthetic model.
    void serve_reads()
    {
        auto reads = std::exchange(pending_reads_, {});
        for (auto const& [target, type, index] : reads) {
            std::vector<uint8_t> descriptor;
            bool ok = true;
            if (type == DESCRIPTOR_ENTITY && index == 0) {
                descriptor = entity_descriptor_bytes();
            } else if (type == DESCRIPTOR_CONFIGURATION && index == 0) {
                descriptor = configuration_descriptor_bytes(control_count, control_block_count);
            } else if (type == DESCRIPTOR_CONTROL && index < control_count) {
                // Indices past the 8 authored shapes fall into
                // control_descriptor's Enable template — enough to bulk a
                // jumbo entity out without inventing 600 distinct controls.
                descriptor = control_descriptor_bytes(index);
            } else if (type == DESCRIPTOR_LOCALE && index == 0) {
                descriptor = locale_descriptor_bytes();
            } else if (type == DESCRIPTOR_STRINGS && index == 0) {
                descriptor = strings_descriptor_bytes();
            } else if (type == DESCRIPTOR_SIGNAL_SELECTOR && index == 0) {
                descriptor = selector_descriptor_bytes();
            } else if (type == DESCRIPTOR_MATRIX && index == 0) {
                descriptor = matrix_descriptor_bytes();
            } else if (type == DESCRIPTOR_CONTROL_BLOCK && index < control_block_count) {
                descriptor = control_block_descriptor_bytes(index);
            } else if (type == DESCRIPTOR_AVB_INTERFACE && index == 0) {
                descriptor = avb_interface_descriptor_bytes();
            } else {
                ok = false;
            }

            AemReadDescriptorCommandPayload sent_cmd{};
            sent_cmd.descriptor_type = type;
            sent_cmd.descriptor_index = index;
            std::array<uint8_t, AemReadDescriptorCommandPayload::LENGTH> sent_bytes{};
            span_store(sent_bytes, sent_cmd);

            std::vector<uint8_t> response(AemReadDescriptorResponsePayload::LENGTH, 0);
            response.insert(response.end(), descriptor.begin(), descriptor.end());

            if (sink_.on_aem_response) {
                sink_.on_aem_response(
                    target,
                    AEM_COMMAND_READ_DESCRIPTOR,
                    ok ? AEM_STATUS_SUCCESS : AEM_STATUS_NO_SUCH_DESCRIPTOR,
                    sent_bytes,
                    make_const_span(response));
            }
        }
    }

    /// Complete the oldest un-completed command of @p code with @p response.
    auto complete(uint16_t code, uint8_t status, std::span<uint8_t const> response) -> bool
    {
        for (auto& s : sent_) {
            if (s.command == code && !s.completed && s.completion) {
                s.completed = true;
                s.completion(atdecc::AemCommandResult{
                    .delivery = atdecc::AemCommandDelivery::Responded,
                    .status = status,
                    .command_type = code,
                    .target_entity_id = s.target,
                    .sent_payload = s.payload,
                    .response = response});
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto sent() -> std::vector<Sent>& { return sent_; }
    [[nodiscard]] auto sink() -> ControllerServiceSink& { return sink_; }

  private:
    struct PendingRead
    {
        Eui64 target{};
        uint16_t type{0};
        uint16_t index{0};
    };

    ControllerServiceSink sink_{};
    std::vector<DiscoveredEntity> entities_;
    std::vector<PendingRead> pending_reads_;
    std::vector<Sent> sent_;
};

/// Labels only for the synthetic entity's Delay Type control — proving
/// both the decoration and that non-enumerated controls are untouched.
class FakeLabels final : public WidgetLabelProvider
{
  public:
    auto options_for(std::uint64_t entity_model_id, std::uint16_t control_index, bool selector)
        -> std::vector<WidgetOption> override
    {
        if (entity_model_id != 0x70B3D5EDC2AA0001ULL || control_index != 4 || selector) {
            return {};
        }
        std::vector<WidgetOption> out;
        char const* const names[] = {"NONE", "BESSEL", "BUTTERWORTH", "LINKWITZ", "ELLIPTIC", "CUSTOM_A", "CUSTOM_B", "CUSTOM_C"};
        for (std::uint16_t i = 0; i < 8; ++i) {
            out.push_back({.value = double(i), .label = names[i]});
        }
        return out;
    }

    auto matrix_row_labels(std::uint64_t entity_model_id, std::uint16_t matrix_index, std::uint16_t row_count)
        -> std::vector<std::string> override
    {
        if (entity_model_id != 0x70B3D5EDC2AA0001ULL || matrix_index != 0 || row_count != 3) {
            return {};
        }
        return {"Mic A", "Mic B", "Playback"};
    }
};

// ---- WS test client ---------------------------------------------------------

struct WsClient
{
    int fd{-1};

    explicit WsClient(net::SocketAddress const& to)
    {
        fd = ::socket(to.family(), SOCK_STREAM, 0);
        if (fd >= 0 && ::connect(fd, to.sockaddr(), to.length()) < 0) {
            ::close(fd);
            fd = -1;
        }
    }
    ~WsClient()
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    WsClient(WsClient const&) = delete;
    auto operator=(WsClient const&) -> WsClient& = delete;

    void send_raw(std::string_view text) const { (void)!::send(fd, text.data(), text.size(), 0); }

    void send_text_frame(std::string_view payload) const
    {
        std::vector<uint8_t> frame;
        frame.push_back(0x81);  // FIN + text
        if (payload.size() <= 125) {
            frame.push_back(uint8_t(0x80 | payload.size()));
        } else {
            frame.push_back(0x80 | 126);
            frame.push_back(uint8_t(payload.size() >> 8));
            frame.push_back(uint8_t(payload.size()));
        }
        std::array<uint8_t, 4> const mask{0x11, 0x22, 0x33, 0x44};
        frame.insert(frame.end(), mask.begin(), mask.end());
        for (size_t i = 0; i < payload.size(); ++i) {
            frame.push_back(uint8_t(payload[i]) ^ mask[i % 4]);
        }
        (void)!::send(fd, frame.data(), frame.size(), 0);
    }

    /// Drains whatever arrives within ~budget_ms into the stream buffer.
    void drain(int budget_ms)
    {
        std::array<uint8_t, 65536> buf{};
        for (int spent = 0; spent < budget_ms; spent += 10) {
            struct pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, 10) <= 0) {
                continue;
            }
            auto const n = ::recv(fd, buf.data(), buf.size(), 0);
            if (n <= 0) {
                break;
            }
            stream.insert(stream.end(), buf.data(), buf.data() + n);
        }
    }

    /// Pops complete text frames from the stream buffer.
    auto take_texts() -> std::vector<std::string>
    {
        std::vector<std::string> out;
        size_t at = 0;
        while (stream.size() - at >= 2) {
            uint8_t const b0 = stream[at];
            uint8_t const b1 = stream[at + 1];
            size_t header = 2;
            uint64_t len = b1 & 0x7FU;
            if (len == 126) {
                if (stream.size() - at < 4) {
                    break;
                }
                len = (uint64_t(stream[at + 2]) << 8) | stream[at + 3];
                header = 4;
            } else if (len == 127) {
                if (stream.size() - at < 10) {
                    break;
                }
                len = 0;
                for (int i = 0; i < 8; ++i) {
                    len = (len << 8) | stream[at + 2 + size_t(i)];
                }
                header = 10;
            }
            if (stream.size() - at < header + len) {
                break;
            }
            if ((b0 & 0x0FU) == 0x1) {
                out.emplace_back(stream.begin() + long(at + header), stream.begin() + long(at + header + len));
            }
            at += header + len;
        }
        stream.erase(stream.begin(), stream.begin() + long(at));
        return out;
    }

    std::vector<uint8_t> stream;
    std::string http_head;
};

// ---- harness ----------------------------------------------------------------

struct Harness
{
    FakeService* fake{};
    FakeLabels labels;
    std::unique_ptr<WidgetBridge> bridge;
    int64_t now = 1'000'000;

    Harness()
    {
        auto svc = std::make_unique<FakeService>();
        fake = svc.get();
        WidgetBridgeConfig config;
        config.limits.max_connections = 4;
        config.labels = &labels;
        bridge = std::make_unique<WidgetBridge>(std::move(svc), config);
        EXPECT_TRUE(bridge->valid());
    }

    /// One reactor-shaped turn: controller tick (drains queued reads),
    /// fake answers, HTTP server pump, bridge pump.
    void turn()
    {
        now += 1'000'000;
        bridge->controller().tick(now);
        fake->serve_reads();
        struct pollfd pfd{.fd = bridge->http_server().fd(), .events = POLLIN, .revents = 0};
        (void)::poll(&pfd, 1, 5);
        bridge->http_server().on_ready(now);
        bridge->http_server().tick(now);
        bridge->pump(now);
    }

    void turns(int n)
    {
        for (int i = 0; i < n; ++i) {
            turn();
        }
    }

    [[nodiscard]] auto addr() -> net::SocketAddress
    {
        auto a = bridge->http_server().local_addr();
        EXPECT_TRUE(a.has_value());
        return *a;
    }
};

template <typename AnyHarness>
auto upgrade(AnyHarness& h, WsClient& client) -> bool
{
    client.send_raw("GET /ws HTTP/1.1\r\n"
                    "Host: bridge\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "\r\n");
    // Collect the 101 head, then leave any frame bytes in the stream.
    std::string head;
    for (int i = 0; i < 40 && head.find("\r\n\r\n") == std::string::npos; ++i) {
        h.turn();
        client.drain(20);
        head.assign(client.stream.begin(), client.stream.end());
    }
    auto const end = head.find("\r\n\r\n");
    if (end == std::string::npos || head.find("101") == std::string::npos) {
        return false;
    }
    client.http_head = head.substr(0, end + 4);
    client.stream.erase(client.stream.begin(), client.stream.begin() + long(end + 4));
    return true;
}

template <typename AnyHarness>
auto collect(AnyHarness& h, WsClient& client, int turns) -> std::vector<std::string>
{
    std::vector<std::string> texts;
    for (int i = 0; i < turns; ++i) {
        h.turn();
        client.drain(10);
        for (auto& t : client.take_texts()) {
            texts.push_back(std::move(t));
        }
    }
    return texts;
}

auto find_op(std::vector<std::string> const& texts, std::string_view op, std::string_view must_contain = {}) -> std::string
{
    for (auto const& t : texts) {
        auto const msg = json_scan(t);
        if (msg.has_value() && msg->string_of("op") == op && (must_contain.empty() || t.find(must_contain) != std::string::npos)) {
            return t;
        }
    }
    return {};
}

}  // namespace

// A JOYNED MU16 answers GET/SET_CONTROL for its 1 x UINT8 IDENTIFY with a
// 13-byte body (value + zero padding); the decoder must take the leading
// element and ignore the tail, and still reject a body that is too short.
TEST(widget_bridge, decode_tolerates_padded_control_body)
{
    DescriptorControl d{};
    d.control_value_type = pack_control_value_type(false, false, CONTROL_LINEAR_UINT8);
    d.number_of_values = 1;
    std::array<uint8_t, 13> padded{0xFF};
    auto const values = decode_control_values(d, make_const_span(padded));
    EXPECT_TRUE(values.has_value() && values->size() == 1 && (*values)[0] == 255.0);

    d.number_of_values = 2;
    std::array<uint8_t, 1> short_body{0x01};
    EXPECT_TRUE(!decode_control_values(d, make_const_span(short_body)).has_value());
    std::array<uint8_t, 3> two_plus_pad{0x01, 0x02, 0x00};
    auto const two = decode_control_values(d, make_const_span(two_plus_pad));
    EXPECT_TRUE(two.has_value() && two->size() == 2 && (*two)[0] == 1.0 && (*two)[1] == 2.0);
}

TEST(widget_bridge, end_to_end_specs_set_unsolicited_departure)
{
    Harness h;

    // -- connect + hello --
    WsClient client{h.addr()};
    EXPECT_TRUE(client.fd >= 0);
    EXPECT_TRUE(upgrade(h, client));
    auto texts = collect(h, client, 3);
    EXPECT_TRUE(!find_op(texts, "hello").empty());

    // -- discovery: crawl completes, specs arrive --
    h.fake->add_entity(DEVICE);
    texts = collect(h, client, 20);
    auto const discovery_texts = texts;  // the full spec replay, kept for later assertions

    auto const entity_msg = find_op(texts, "entity");
    EXPECT_TRUE(!entity_msg.empty());
    if (!entity_msg.empty()) {
        auto const parsed = json_scan(entity_msg);
        EXPECT_TRUE(parsed->hex64_of("entity") == DEVICE.to_uint64());
        EXPECT_TRUE(parsed->string_of("name") == "Widget Test Box");
        EXPECT_TRUE(parsed->number_of("widgets") == 10.0);
        // ENTITY-descriptor detail: inline fields plus the localized vendor.
        EXPECT_TRUE(parsed->string_of("firmware") == "1.2.3");
        EXPECT_TRUE(parsed->string_of("serial") == "SN-4242");
        EXPECT_TRUE(parsed->string_of("group") == "FOH");
        EXPECT_TRUE(parsed->string_of("vendor") == "Statusbar Test");
        EXPECT_TRUE(parsed->number_of("talkers") == 4.0);
        EXPECT_TRUE(parsed->number_of("listeners") == 2.0);
        EXPECT_TRUE(parsed->number_of("configs") == 1.0);
    }

    auto const gain = json_scan(find_op(texts, "widget", "\"Gain\""));
    EXPECT_TRUE(gain.has_value());
    if (gain.has_value()) {
        EXPECT_TRUE(gain->string_of("kind") == "slider");
        EXPECT_TRUE(gain->number_of("min") == -90.0);
        // The hierarchy metadata: containing block + control type.
        EXPECT_TRUE(gain->string_of("group") == "Input 1");
        EXPECT_TRUE(gain->hex64_of("ct") == 0x90E0F00000000004ULL);
        EXPECT_TRUE(gain->number_of("max") == 10.0);
        EXPECT_TRUE(gain->number_of("step") == 0.5);
        EXPECT_TRUE(gain->number_of("control") == 0.0);
        EXPECT_EQ(gain->array_key, std::string{"values"});
        EXPECT_TRUE(gain->array_values.size() == 1 && gain->array_values[0] == -12.0);
        auto const symbol = gain->string_of("symbol");
        EXPECT_TRUE(symbol.has_value() && !symbol->empty());
    }

    auto const mode = json_scan(find_op(texts, "widget", "\"Mode\""));
    EXPECT_TRUE(mode.has_value());
    if (mode.has_value()) {
        EXPECT_TRUE(mode->string_of("kind") == "select");
        EXPECT_EQ(mode->array_key, std::string{"options"});
        EXPECT_TRUE(mode->array_values.size() == 3);
        EXPECT_TRUE(mode->number_of("value") == 1.0);
    }

    // Control 6: the Milan-mandated IDENTIFY projects as its own kind.
    // The display name prefers the object_name sticker ("Blink"); the
    // LOCALE-chased localized string ("Identify") travels as "mfr" so
    // the site can toggle to factory names.
    auto const identify = json_scan(find_op(texts, "widget", "\"Blink\""));
    EXPECT_TRUE(identify.has_value());
    if (identify.has_value()) {
        EXPECT_TRUE(identify->string_of("kind") == "identify");
        EXPECT_TRUE(identify->string_of("name") == "Blink");
        EXPECT_TRUE(identify->string_of("mfr") == "Identify");
        EXPECT_TRUE(identify->number_of("control") == 6.0);
    }

    auto const enable = json_scan(find_op(texts, "widget", "\"Enable\""));
    EXPECT_TRUE(enable.has_value());
    if (enable.has_value()) {
        EXPECT_TRUE(enable->string_of("kind") == "toggle");
    }

    // The AEM 0/255 two-state convention is a toggle carrying its own
    // on/off values...
    auto const mute = json_scan(find_op(texts, "widget", "\"Mute\""));
    EXPECT_TRUE(mute.has_value());
    if (mute.has_value()) {
        EXPECT_TRUE(mute->string_of("kind") == "toggle");
        EXPECT_TRUE(mute->string_of("group") == "Input 1");
        EXPECT_TRUE(mute->hex64_of("ct") == 0x90E0F00000000002ULL);
        EXPECT_TRUE(mute->number_of("min") == 0.0);
        EXPECT_TRUE(mute->number_of("max") == 255.0);
        EXPECT_TRUE(mute->array_values.size() == 1 && mute->array_values[0] == 255.0);
    }

    // ...and a small discrete integer linear is a select over its grid.
    auto const delay_type = json_scan(find_op(texts, "widget", "\"Delay Type\""));
    EXPECT_TRUE(delay_type.has_value());
    if (delay_type.has_value()) {
        EXPECT_TRUE(delay_type->string_of("kind") == "select");
        EXPECT_EQ(delay_type->array_key, std::string{"options"});
        EXPECT_EQ(delay_type->array_values.size(), 8U);
        EXPECT_TRUE(delay_type->array_values.front() == 0.0 && delay_type->array_values.back() == 7.0);
        EXPECT_TRUE(delay_type->number_of("value") == 2.0);
        // The label provider's names ride beside the options.
        auto const* labels = delay_type->array_of("labels");
        EXPECT_TRUE(labels != nullptr);
        if (labels != nullptr) {
            EXPECT_EQ(labels->strings.size(), 8U);
            EXPECT_TRUE(labels->strings[1] == "BESSEL" && labels->strings[7] == "CUSTOM_C");
        }
    }

    // Controls the provider does not enumerate carry no labels.
    EXPECT_TRUE(mute.has_value() && mute->array_of("labels") == nullptr);

    // The array control projects as a read-only meter bank with units.
    auto const levels = json_scan(find_op(texts, "widget", "\"Levels\""));
    EXPECT_TRUE(levels.has_value());
    if (levels.has_value()) {
        EXPECT_TRUE(levels->string_of("kind") == "meter");
        EXPECT_TRUE(levels->find("ro") != nullptr && levels->find("ro")->bool_value);
        EXPECT_TRUE(levels->number_of("min") == -90.0 && levels->number_of("max") == 20.0);
        EXPECT_TRUE(levels->number_of("unit") == 176.0);
        EXPECT_TRUE(levels->find("group") == nullptr);  // block-less
        auto const* level_values = levels->array_of("values");
        EXPECT_TRUE(level_values != nullptr && level_values->numbers.size() == 2 && level_values->numbers[0] == -26.5);
    }

    // A multi-value linear control ships one localized label per value
    // (an unauthored slot is "" so the array stays parallel to `values`).
    auto const temperature = json_scan(find_op(texts, "widget", "\"Temperature\""));
    EXPECT_TRUE(temperature.has_value());
    if (temperature.has_value()) {
        EXPECT_TRUE(temperature->string_of("kind") == "slider");
        EXPECT_TRUE(temperature->find("ro") != nullptr && temperature->find("ro")->bool_value);
        EXPECT_TRUE(temperature->number_of("unit_multiplier") == -2.0);
        auto const* labels = temperature->array_of("labels");
        EXPECT_TRUE(labels != nullptr);
        if (labels != nullptr) {
            EXPECT_EQ(labels->strings.size(), 3U);
            EXPECT_TRUE(labels->strings[0] == "NVME" && labels->strings[1] == "CPU" && labels->strings[2].empty());
        }
        auto const* currents = temperature->array_of("values");
        EXPECT_TRUE(currents != nullptr && currents->numbers.size() == 3 && currents->numbers[1] == 7200.0);
    }

    EXPECT_TRUE(!find_op(texts, "entity_done").empty());

    // The bridge registered for unsolicited notifications.
    bool registered = false;
    for (auto const& s : h.fake->sent()) {
        registered = registered || (s.command == AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION && s.target == DEVICE);
    }
    EXPECT_TRUE(registered);

    // -- SET round trip: client sets gain, device confirms, value fans out --
    client.send_text_frame(R"({"op":"set","entity":"0x70b3d5edc2000042","control":0,"values":[-6]})");
    h.turns(3);

    FakeService::Sent* set_cmd = nullptr;
    for (auto& s : h.fake->sent()) {
        if (s.command == AEM_COMMAND_SET_CONTROL) {
            set_cmd = &s;
        }
    }
    EXPECT_TRUE(set_cmd != nullptr);
    if (set_cmd != nullptr) {
        // Payload: control header (CONTROL, index 0) + float -6.0 big-endian.
        std::vector<uint8_t> const expected{0x00, 0x1A, 0x00, 0x00, 0xC0, 0xC0, 0x00, 0x00};
        EXPECT_TRUE(set_cmd->payload == expected);

        (void)h.fake->complete(AEM_COMMAND_SET_CONTROL, AEM_STATUS_SUCCESS, set_cmd->payload);
    }
    texts = collect(h, client, 3);
    auto const confirmed = json_scan(find_op(texts, "value"));
    EXPECT_TRUE(confirmed.has_value());
    if (confirmed.has_value()) {
        EXPECT_TRUE(confirmed->number_of("control") == 0.0);
        EXPECT_TRUE(confirmed->array_values.size() == 1 && confirmed->array_values[0] == -6.0);
    }

    // -- a rejected SET reports an error --
    client.send_text_frame(R"({"op":"set","entity":"0x70b3d5edc2000042","control":0,"values":[999]})");
    texts = collect(h, client, 3);
    EXPECT_TRUE(!find_op(texts, "error", "out of range").empty());

    // -- unsolicited: another controller flips Enable; every client hears --
    std::array<uint8_t, 5> const unsolicited_body{0x00, 0x1A, 0x00, 0x02, 0x01};
    h.fake->sink().on_unsolicited(DEVICE, AEM_COMMAND_SET_CONTROL, AEM_STATUS_SUCCESS, unsolicited_body);
    texts = collect(h, client, 3);
    auto const flipped = json_scan(find_op(texts, "value", "\"control\":2"));
    EXPECT_TRUE(flipped.has_value());
    if (flipped.has_value()) {
        EXPECT_TRUE(flipped->array_values.size() == 1 && flipped->array_values[0] == 1.0);
    }

    // -- SIGNAL_SELECTOR: authored sources become a labeled select in its
    // own index namespace ("d":"sel"), and the round trip maps option
    // index <-> source triple --
    auto const source_sel = json_scan(find_op(discovery_texts, "widget", "\"Input Source\""));
    EXPECT_TRUE(source_sel.has_value());
    if (source_sel.has_value()) {
        EXPECT_TRUE(source_sel->string_of("d") == "sel");
        EXPECT_TRUE(source_sel->string_of("kind") == "select");
        EXPECT_TRUE(source_sel->number_of("control") == 0.0);
        auto const* options = source_sel->array_of("options");
        auto const* labels = source_sel->array_of("labels");
        EXPECT_TRUE(options != nullptr && options->numbers.size() == 2);
        EXPECT_TRUE(labels != nullptr && labels->strings.size() == 2);
        EXPECT_TRUE(source_sel->number_of("value") == 1.0);  // current = second source
        EXPECT_FALSE(source_sel->find("ro") != nullptr && source_sel->find("ro")->bool_value);
    }

    // Select the first source; the fake backend sees the raw triple.
    client.send_text_frame(R"({"op":"set","entity":"0x70b3d5edc2000042","control":0,"d":"sel","values":[0]})");
    h.turns(3);
    FakeService::Sent* sel_set = nullptr;
    for (auto& s : h.fake->sent()) {
        if (s.command == AEM_COMMAND_SET_SIGNAL_SELECTOR) {
            sel_set = &s;
        }
    }
    EXPECT_TRUE(sel_set != nullptr);
    if (sel_set != nullptr) {
        std::vector<uint8_t> const jack0{
            uint8_t(DESCRIPTOR_JACK_INPUT >> 8), uint8_t(DESCRIPTOR_JACK_INPUT), 0x00, 0x00, 0x00, 0x00};
        EXPECT_TRUE(sel_set->payload == jack0);

        // Confirming response: AemSignalSelectorPayload echoing source 0.
        AemSignalSelectorPayload confirm{};
        confirm.descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR;
        confirm.descriptor_index = 0;
        confirm.source = selector_source(0);
        (void)h.fake->complete(AEM_COMMAND_SET_SIGNAL_SELECTOR, AEM_STATUS_SUCCESS, make_const_span(confirm));
    }
    texts = collect(h, client, 3);
    auto const sel_value = json_scan(find_op(texts, "value", "\"d\":\"sel\""));
    EXPECT_TRUE(sel_value.has_value());
    if (sel_value.has_value()) {
        EXPECT_TRUE(sel_value->number_of("control") == 0.0);
        EXPECT_TRUE(sel_value->array_values.size() == 1 && sel_value->array_values[0] == 0.0);
    }

    // Unsolicited selector change from another controller: back to source 1.
    {
        AemSignalSelectorPayload changed{};
        changed.descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR;
        changed.descriptor_index = 0;
        changed.source = selector_source(1);
        h.fake->sink().on_unsolicited(DEVICE, AEM_COMMAND_SET_SIGNAL_SELECTOR, AEM_STATUS_SUCCESS, make_const_span(changed));
    }
    texts = collect(h, client, 3);
    auto const sel_unsol = json_scan(find_op(texts, "value", "\"d\":\"sel\""));
    EXPECT_TRUE(sel_unsol.has_value());
    if (sel_unsol.has_value()) {
        EXPECT_TRUE(sel_unsol->array_values.size() == 1 && sel_unsol->array_values[0] == 1.0);
    }

    // -- MATRIX: grid spec, chunked load, cell set, unsolicited region --
    auto const routing = json_scan(find_op(discovery_texts, "widget", "\"Routing\""));
    EXPECT_TRUE(routing.has_value());
    if (routing.has_value()) {
        EXPECT_TRUE(routing->string_of("d") == "mx");
        EXPECT_TRUE(routing->string_of("kind") == "matrix");
        EXPECT_TRUE(routing->number_of("width") == 4.0 && routing->number_of("height") == 3.0);
        EXPECT_TRUE(routing->number_of("min") == -90.0 && routing->number_of("max") == 10.0);
        EXPECT_TRUE(routing->number_of("unit") == 176.0);
        auto const* grid = routing->array_of("values");
        EXPECT_TRUE(grid != nullptr && grid->numbers.size() == 12);
        auto const* rows = routing->array_of("rows");
        EXPECT_TRUE(rows != nullptr);
        if (rows != nullptr) {
            EXPECT_TRUE(rows->strings.size() == 3 && rows->strings[0] == "Mic A" && rows->strings[2] == "Playback");
        }
    }

    // The bridge asked for the whole grid in one region (12 cells fit).
    FakeService::Sent* grid_get = nullptr;
    for (auto& s : h.fake->sent()) {
        if (s.command == AEM_COMMAND_GET_MATRIX && !s.completed) {
            grid_get = &s;
        }
    }
    EXPECT_TRUE(grid_get != nullptr);
    if (grid_get != nullptr) {
        std::vector<uint8_t> const expected_region{
            0x00, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00};
        EXPECT_TRUE(grid_get->payload == expected_region);
        std::vector<float> cells;
        for (int i = 0; i < 12; ++i) {
            cells.push_back(float(-i));
        }
        (void)h.fake->complete(AEM_COMMAND_GET_MATRIX, AEM_STATUS_SUCCESS, make_const_span(matrix_response(0, 0, 4, 3, cells)));
    }
    texts = collect(h, client, 3);
    auto const grid_push = json_scan(find_op(texts, "value", "\"d\":\"mx\""));
    EXPECT_TRUE(grid_push.has_value());
    if (grid_push.has_value()) {
        EXPECT_TRUE(grid_push->number_of("at") == 0.0);
        auto const* run = grid_push->array_of("values");
        EXPECT_TRUE(run != nullptr && run->numbers.size() == 12 && run->numbers[5] == -5.0);
    }

    // Set one crosspoint: at 6 = row 1, column 2.
    client.send_text_frame(R"({"op":"set","entity":"0x70b3d5edc2000042","control":0,"d":"mx","at":6,"values":[-3]})");
    h.turns(3);
    FakeService::Sent* cell_set = nullptr;
    for (auto& s : h.fake->sent()) {
        if (s.command == AEM_COMMAND_SET_MATRIX) {
            cell_set = &s;
        }
    }
    EXPECT_TRUE(cell_set != nullptr);
    if (cell_set != nullptr) {
        std::vector<uint8_t> const expected{0x00, 0x1D, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01,
                                            0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xC0, 0x40, 0x00, 0x00};
        EXPECT_TRUE(cell_set->payload == expected);
        (void)h.fake->complete(AEM_COMMAND_SET_MATRIX, AEM_STATUS_SUCCESS, make_const_span(matrix_response(2, 1, 1, 1, {-3.0F})));
    }
    texts = collect(h, client, 3);
    auto const cell_push = json_scan(find_op(texts, "value", "\"at\":6"));
    EXPECT_TRUE(cell_push.has_value());
    if (cell_push.has_value()) {
        EXPECT_TRUE(cell_push->array_values.size() == 1 && cell_push->array_values[0] == -3.0);
    }

    // Unsolicited region from another controller: cell 11 (row 2, col 3).
    h.fake->sink().on_unsolicited(
        DEVICE, AEM_COMMAND_SET_MATRIX, AEM_STATUS_SUCCESS, make_const_span(matrix_response(3, 2, 1, 1, {5.0F})));
    texts = collect(h, client, 3);
    auto const unsol_push = json_scan(find_op(texts, "value", "\"at\":11"));
    EXPECT_TRUE(unsol_push.has_value());
    if (unsol_push.has_value()) {
        EXPECT_TRUE(unsol_push->array_values.size() == 1 && unsol_push->array_values[0] == 5.0);
    }

    // -- live-value sweep: registration confirmation triggers paced
    // GET_CONTROLs; changed values push, unchanged ones are suppressed --
    texts = collect(h, client, 2);  // drain anything pending
    EXPECT_TRUE(h.fake->complete(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, AEM_STATUS_SUCCESS, {}));
    std::set<uint16_t> swept;
    for (int round = 0; round < 8; ++round) {
        h.turns(2);
        bool completed_one = false;
        for (auto& sent : h.fake->sent()) {
            if (sent.command != AEM_COMMAND_GET_CONTROL || sent.completed || !sent.completion) {
                continue;
            }
            if (sent.payload.size() < 4) {
                continue;
            }
            auto const index = uint16_t((sent.payload[2] << 8) | sent.payload[3]);
            swept.insert(index);
            // Live values: Gain answers a genuinely different -24 dB;
            // everything else echoes its current (suppressed).
            std::vector<uint8_t> response{0x00, 0x1A, uint8_t(index >> 8), uint8_t(index & 0xFF)};
            auto push_float = [&](float v) {
                std::array<uint8_t, 4> b{};
                detail::encode_linear_field(v, b);
                response.insert(response.end(), b.begin(), b.end());
            };
            switch (index) {
                case 0:
                    push_float(-24.0F);
                    break;
                case 1:
                    response.push_back(0x00);
                    response.push_back(0x01);
                    break;
                case 2:
                    response.push_back(0x01);  // Enable: flipped earlier by unsolicited
                    break;
                case 3:
                    response.push_back(0xFF);  // Mute current
                    break;
                case 4:
                    response.push_back(0x02);  // Delay Type current
                    break;
                case 5:
                    push_float(-26.5F);  // Levels currents
                    push_float(-40.0F);
                    break;
                case 7:
                    for (uint16_t const v : {uint16_t{4584}, uint16_t{7200}, uint16_t{4700}}) {  // Temperature currents
                        response.push_back(uint8_t(v >> 8));
                        response.push_back(uint8_t(v & 0xFF));
                    }
                    break;
                default:
                    break;
            }
            sent.completed = true;
            auto completion = std::move(sent.completion);
            completion(atdecc::AemCommandResult{
                .delivery = atdecc::AemCommandDelivery::Responded,
                .status = AEM_STATUS_SUCCESS,
                .command_type = AEM_COMMAND_GET_CONTROL,
                .target_entity_id = DEVICE,
                .sent_payload = sent.payload,
                .response = response});
            completed_one = true;
        }
        if (!completed_one && swept.size() >= 8) {
            break;
        }
    }
    EXPECT_EQ(swept.size(), 8U);  // controls 0-7, nothing for text/opaque kinds
    texts = collect(h, client, 3);
    auto const live_gain = json_scan(find_op(texts, "value", "\"control\":0,\"values\""));
    EXPECT_TRUE(live_gain.has_value());
    if (live_gain.has_value()) {
        EXPECT_TRUE(live_gain->array_values.size() == 1 && live_gain->array_values[0] == -24.0);
    }
    // Unchanged sweep answers stay quiet.
    EXPECT_TRUE(find_op(texts, "value", "\"control\":4").empty());
    EXPECT_TRUE(find_op(texts, "value", "\"control\":3,\"values\"").empty());

    // -- departure --
    h.fake->remove_entity(DEVICE);
    texts = collect(h, client, 3);
    EXPECT_TRUE(!find_op(texts, "departed").empty());
    EXPECT_TRUE(h.bridge->entities().empty());
}

TEST(widget_bridge, avb_interface_detail_and_live_gptp_state)
{
    Harness h;
    WsClient client{h.addr()};
    EXPECT_TRUE(client.fd >= 0);
    EXPECT_TRUE(upgrade(h, client));
    (void)collect(h, client, 3);

    h.fake->add_entity(DEVICE);
    auto texts = collect(h, client, 20);

    // The descriptor's static half rides as an "interface" frame.
    auto const iface = json_scan(find_op(texts, "interface"));
    EXPECT_TRUE(iface.has_value());
    if (iface.has_value()) {
        EXPECT_TRUE(iface->hex64_of("entity") == DEVICE.to_uint64());
        EXPECT_TRUE(iface->number_of("interface") == 0.0);
        EXPECT_TRUE(iface->string_of("name") == "eth0");
        EXPECT_TRUE(iface->string_of("mac") == "02:11:22:33:44:55");
        EXPECT_TRUE(iface->hex64_of("clock_id") == 0x021122FFFE334455ULL);
        EXPECT_TRUE(iface->number_of("priority1") == 248.0);
        EXPECT_TRUE(iface->number_of("priority2") == 200.0);
        EXPECT_TRUE(iface->number_of("log_sync") == -3.0);
        EXPECT_TRUE(iface->number_of("port") == 1.0);
    }

    // Registration confirmed => the bridge reads the live state, and
    // re-reads the descriptor (a proxy's gPTP fields settle after its
    // crawl).
    EXPECT_TRUE(h.fake->complete(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, AEM_STATUS_SUCCESS, {}));
    h.turns(2);
    bool asked = false;
    bool reread = false;
    for (auto const& s : h.fake->sent()) {
        if (s.command == AEM_COMMAND_GET_AVB_INFO && s.payload.size() == 4 && s.payload[1] == 0x09) {
            asked = true;
        }
        if (s.command == AEM_COMMAND_READ_DESCRIPTOR && s.payload.size() == AemReadDescriptorCommandPayload::LENGTH &&
            s.payload[5] == 0x09 && s.payload[7] == 0) {
            reread = true;
        }
    }
    EXPECT_TRUE(asked);
    EXPECT_TRUE(reread);

    // The re-read descriptor carries the settled gPTP fields: a changed
    // clock identity and priority1 push a fresh "interface" frame.
    {
        DescriptorAvbInterface d{};
        auto const base = avb_interface_descriptor_bytes();
        span_load_padded(d, make_const_span(base));
        d.clock_identity = Eui64{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0xC8, 0xF0};
        d.priority1 = 246;
        std::vector<uint8_t> reread_response(AemReadDescriptorResponsePayload::LENGTH, 0);
        auto const bytes = make_const_span(d).first(DescriptorAvbInterface::LENGTH);
        reread_response.insert(reread_response.end(), bytes.begin(), bytes.end());
        EXPECT_TRUE(h.fake->complete(AEM_COMMAND_READ_DESCRIPTOR, AEM_STATUS_SUCCESS, reread_response));
        auto const after = collect(h, client, 4);
        auto const settled = json_scan(find_op(after, "interface"));
        EXPECT_TRUE(settled.has_value());
        if (settled.has_value()) {
            EXPECT_TRUE(settled->hex64_of("clock_id") == 0x001CABFFFE00C8F0ULL);
            EXPECT_TRUE(settled->number_of("priority1") == 246.0);
            EXPECT_TRUE(settled->string_of("name") == "eth0");
        }
    }

    // The answer becomes an "avb_info" frame.
    AemAvbInfoPayload info{};
    info.descriptor_type = DESCRIPTOR_AVB_INTERFACE;
    info.descriptor_index = 0;
    info.gptp_grandmaster_id = tsn::ClockIdentity{Eui64{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x76, 0x04}};
    info.propagation_delay = 412;
    info.gptp_domain_number = 0;
    info.flags =
        avb_info_flags::AS_CAPABLE | avb_info_flags::GPTP_ENABLED | avb_info_flags::SRP_ENABLED | avb_info_flags::AVTP_DOWN_VALID;
    info.msrp_mappings_count = 1;
    std::vector<uint8_t> response(AemAvbInfoPayload::LENGTH + 4, 0);
    span_store(std::span<uint8_t>{response}.first(AemAvbInfoPayload::LENGTH), info);
    response[AemAvbInfoPayload::LENGTH + 0] = 0;  // class A
    response[AemAvbInfoPayload::LENGTH + 1] = 3;  // pcp 3
    response[AemAvbInfoPayload::LENGTH + 3] = 2;  // vlan 2
    EXPECT_TRUE(h.fake->complete(AEM_COMMAND_GET_AVB_INFO, AEM_STATUS_SUCCESS, response));
    texts = collect(h, client, 4);
    auto const live = json_scan(find_op(texts, "avb_info"));
    EXPECT_TRUE(live.has_value());
    if (live.has_value()) {
        EXPECT_TRUE(live->hex64_of("gm") == 0x001CABFFFE007604ULL);
        EXPECT_TRUE(live->number_of("prop_delay") == 412.0);
    }
    auto const live_text = find_op(texts, "avb_info");
    EXPECT_TRUE(live_text.find("\"as_capable\":true") != std::string::npos);
    EXPECT_TRUE(live_text.find("\"srp_enabled\":true") != std::string::npos);
    EXPECT_TRUE(live_text.find("\"avtp_down\":false") != std::string::npos);
    if (live.has_value()) {
        auto const* tc = live->array_of("msrp_tc");
        auto const* vlan = live->array_of("msrp_vlan");
        EXPECT_TRUE(tc != nullptr && tc->numbers.size() == 1 && tc->numbers[0] == 0.0);
        EXPECT_TRUE(vlan != nullptr && vlan->numbers.size() == 1 && vlan->numbers[0] == 2.0);
    }

    // An unsolicited GET_AVB_INFO with a new grandmaster pushes again...
    info.gptp_grandmaster_id = tsn::ClockIdentity{Eui64{0x00, 0x01, 0xF2, 0xFF, 0xFE, 0x00, 0x68, 0x82}};
    span_store(std::span<uint8_t>{response}.first(AemAvbInfoPayload::LENGTH), info);
    h.fake->sink().on_unsolicited(DEVICE, AEM_COMMAND_GET_AVB_INFO, AEM_STATUS_SUCCESS, response);
    texts = collect(h, client, 4);
    auto const pushed = json_scan(find_op(texts, "avb_info"));
    EXPECT_TRUE(pushed.has_value() && pushed->hex64_of("gm") == 0x0001F2FFFE006882ULL);

    // ...and a late client gets the interface and its live state on replay.
    WsClient late{h.addr()};
    EXPECT_TRUE(upgrade(h, late));
    auto const replay = collect(h, late, 20);
    EXPECT_TRUE(!find_op(replay, "interface").empty());
    auto const replayed = json_scan(find_op(replay, "avb_info"));
    EXPECT_TRUE(replayed.has_value() && replayed->hex64_of("gm") == 0x0001F2FFFE006882ULL);
}

TEST(widget_bridge, late_client_gets_replay_and_refresh)
{
    Harness h;
    h.fake->add_entity(DEVICE);
    h.turns(20);  // crawl completes with no clients connected
    EXPECT_EQ(h.bridge->entities().size(), 1U);

    WsClient late{h.addr()};
    EXPECT_TRUE(late.fd >= 0);
    EXPECT_TRUE(upgrade(h, late));
    auto texts = collect(h, late, 5);
    EXPECT_TRUE(!find_op(texts, "hello").empty());
    EXPECT_TRUE(!find_op(texts, "entity").empty());
    EXPECT_TRUE(!find_op(texts, "widget", "\"Gain\"").empty());
    EXPECT_TRUE(!find_op(texts, "entity_done").empty());

    // refresh replays on demand.
    late.send_text_frame(R"({"op":"refresh"})");
    texts = collect(h, late, 5);
    EXPECT_TRUE(!find_op(texts, "entity").empty());

    // A malformed message is answered, not dropped.
    late.send_text_frame("{nope");
    texts = collect(h, late, 3);
    EXPECT_TRUE(!find_op(texts, "error", "malformed").empty());
}

namespace {

/// The synthetic model as a `.aem` blob — what the offline mode loads.
auto synthetic_blob() -> std::vector<uint8_t>
{
    nanoavb::AemBlobWriter writer;
    writer.add(0, DESCRIPTOR_ENTITY, 0, entity_descriptor_bytes());
    writer.add(0, DESCRIPTOR_CONFIGURATION, 0, configuration_descriptor_bytes());
    for (uint16_t i = 0; i < 8; ++i) {
        writer.add(0, DESCRIPTOR_CONTROL, i, control_descriptor_bytes(i));
    }
    writer.add(0, DESCRIPTOR_LOCALE, 0, locale_descriptor_bytes());
    writer.add(0, DESCRIPTOR_STRINGS, 0, strings_descriptor_bytes());
    writer.add(0, DESCRIPTOR_SIGNAL_SELECTOR, 0, selector_descriptor_bytes());
    writer.add(0, DESCRIPTOR_MATRIX, 0, matrix_descriptor_bytes());
    writer.add(0, DESCRIPTOR_CONTROL_BLOCK, 0, control_block_descriptor_bytes());
    return writer.write();
}

struct OfflineHarness
{
    std::unique_ptr<WidgetBridge> bridge;
    int64_t now = 1'000'000;

    OfflineHarness()
    {
        auto service = std::make_unique<OfflineControllerService>(synthetic_blob());
        EXPECT_TRUE(service->valid());
        WidgetBridgeConfig config;
        config.limits.max_connections = 4;
        bridge = std::make_unique<WidgetBridge>(std::move(service), config);
        EXPECT_TRUE(bridge->valid());
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
    void turns(int n)
    {
        for (int i = 0; i < n; ++i) {
            turn();
        }
    }
    [[nodiscard]] auto addr() -> net::SocketAddress
    {
        auto a = bridge->http_server().local_addr();
        EXPECT_TRUE(a.has_value());
        return *a;
    }
};

}  // namespace

TEST(widget_bridge, offline_mode_serves_and_persists)
{
    OfflineHarness h;
    WsClient client{h.addr()};
    EXPECT_TRUE(client.fd >= 0);
    EXPECT_TRUE(upgrade(h, client));

    // The in-process model discovers, crawls, and builds like a live one.
    auto texts = collect(h, client, 60);
    auto const entity_msg = json_scan(find_op(texts, "entity"));
    EXPECT_TRUE(entity_msg.has_value());
    if (entity_msg.has_value()) {
        EXPECT_TRUE(entity_msg->string_of("name") == "Widget Test Box");
        EXPECT_TRUE(entity_msg->number_of("widgets") == 10.0);
    }
    auto const gain = json_scan(find_op(texts, "widget", "\"Gain\""));
    EXPECT_TRUE(gain.has_value());
    if (gain.has_value()) {
        EXPECT_TRUE(gain->string_of("group") == "Input 1");
        EXPECT_TRUE(gain->array_values.size() == 1 && gain->array_values[0] == -12.0);  // authored current
    }
    EXPECT_TRUE(!find_op(texts, "entity_done").empty());

    // SET persists in the model: the confirmation echoes, and a fresh
    // GET reads the stored value back.
    client.send_text_frame(R"({"op":"set","entity":"0x70b3d5edc2000042","control":0,"values":[-6]})");
    texts = collect(h, client, 6);
    auto const confirmed = json_scan(find_op(texts, "value", "\"control\":0,\"values\""));
    EXPECT_TRUE(confirmed.has_value());
    if (confirmed.has_value()) {
        EXPECT_TRUE(confirmed->array_values.size() == 1 && confirmed->array_values[0] == -6.0);
    }
    // A client GET always answers, even though the stored -6 equals the
    // spec's current (only the sweep and unsolicited pushes suppress
    // unchanged values) — the readback is the persistence proof.
    client.send_text_frame(R"({"op":"get","entity":"0x70b3d5edc2000042","control":0})");
    texts = collect(h, client, 6);
    auto const readback = json_scan(find_op(texts, "value", "\"control\":0,\"values\""));
    EXPECT_TRUE(readback.has_value());
    if (readback.has_value()) {
        EXPECT_TRUE(readback->array_values.size() == 1 && readback->array_values[0] == -6.0);
    }
    client.send_text_frame(R"({"op":"set","entity":"0x70b3d5edc2000042","control":0,"values":[-7]})");
    texts = collect(h, client, 6);
    auto const again = json_scan(find_op(texts, "value", "\"control\":0,\"values\""));
    EXPECT_TRUE(again.has_value() && again->array_values.size() == 1 && again->array_values[0] == -7.0);

    // Matrix cell set persists and fans out through the same pipeline.
    client.send_text_frame(R"({"op":"set","entity":"0x70b3d5edc2000042","control":0,"d":"mx","at":6,"values":[-3]})");
    texts = collect(h, client, 6);
    auto const cell = json_scan(find_op(texts, "value", "\"at\":6"));
    EXPECT_TRUE(cell.has_value() && cell->array_values.size() == 1 && cell->array_values[0] == -3.0);

    // Selector set round-trips against the authored source list.
    client.send_text_frame(R"({"op":"set","entity":"0x70b3d5edc2000042","control":0,"d":"sel","values":[0]})");
    texts = collect(h, client, 6);
    auto const sel = json_scan(find_op(texts, "value", "\"d\":\"sel\""));
    EXPECT_TRUE(sel.has_value() && sel->array_values.size() == 1 && sel->array_values[0] == 0.0);

    // Rename: SET_NAME round-trips through the model and the updated
    // spec fans out — the sticker wins the display name.
    client.send_text_frame(R"({"op":"rename","entity":"0x70b3d5edc2000042","control":0,"name":"Lead Vocal"})");
    texts = collect(h, client, 6);
    auto const renamed = json_scan(find_op(texts, "widget", "\"Lead Vocal\""));
    EXPECT_TRUE(renamed.has_value());
    if (renamed.has_value()) {
        EXPECT_TRUE(renamed->string_of("name") == "Lead Vocal");
        EXPECT_TRUE(renamed->number_of("control") == 0.0);
    }

    // READ_DESCRIPTOR agrees: a fresh crawl of the renamed CONTROL
    // carries the sticker (the offline handler reflects the override).
    // Then an empty rename clears back to the authored object_name.
    client.send_text_frame(R"({"op":"rename","entity":"0x70b3d5edc2000042","control":0,"name":""})");
    texts = collect(h, client, 6);
    auto const cleared = json_scan(find_op(texts, "widget", "\"name\":\"Gain\""));
    EXPECT_TRUE(cleared.has_value());
    if (cleared.has_value()) {
        EXPECT_TRUE(cleared->string_of("name") == "Gain");
    }
}

//
// Test Runner
//

// An entity past JUMBO_WIDGET_THRESHOLD (the nadia-CP proxy serves 55k
// CONTROLs) must not dump every spec at every client: discovery sends a
// group summary, a client opens the groups it is looking at, and live
// value pushes reach only clients with that group open.
TEST(widget_bridge, jumbo_entity_sends_groups_and_scopes_replay)
{
    Harness h;
    h.fake->control_count = 600;        // 600 CONTROLs + selector + matrix = 602 widgets
    h.fake->control_block_count = 300;  // "Input 1" + 299 one-control "Blk k" groups

    WsClient client{h.addr()};
    EXPECT_TRUE(client.fd >= 0);
    EXPECT_TRUE(upgrade(h, client));
    (void)collect(h, client, 3);

    // -- discovery: the summary arrives, not 602 specs --
    h.fake->add_entity(DEVICE);
    auto texts = collect(h, client, 30);
    auto const entity_msg = json_scan(find_op(texts, "entity"));
    EXPECT_TRUE(entity_msg.has_value());
    if (entity_msg.has_value()) {
        EXPECT_TRUE(entity_msg->number_of("widgets") == 602.0);
    }
    EXPECT_TRUE(find_op(texts, "widget").empty());
    // The groups payload is an array of objects, past json_scan's flat
    // grammar — match the raw text. 301 groups exceed one frame budget,
    // so the summary spans several seq-numbered frames ending in
    // "last":true, each one small enough for the server's tx buffer.
    std::vector<std::string> groups_frames;
    std::string joined;
    for (auto const& t : texts) {
        if (t.find(R"("op":"groups")") != std::string::npos) {
            groups_frames.push_back(t);
            joined += t;
        }
    }
    EXPECT_TRUE(groups_frames.size() >= 2);
    for (auto const& frame : groups_frames) {
        EXPECT_TRUE(frame.size() < 4096 - 16);
    }
    EXPECT_TRUE(!groups_frames.empty() && groups_frames.front().find(R"("seq":0)") != std::string::npos);
    EXPECT_TRUE(!groups_frames.empty() && groups_frames.back().find(R"("last":true)") != std::string::npos);
    // CONTROL_BLOCK "Input 1" spans controls 0..4, "Blk k" covers control
    // 7+k, and the remaining controls plus the selector and matrix ride
    // block-less in the "" bucket: 602 - 5 - 299 = 298.
    EXPECT_TRUE(joined.find(R"("name":"Input 1","count":5)") != std::string::npos);
    EXPECT_TRUE(joined.find(R"("name":"Blk 299","count":1)") != std::string::npos);
    EXPECT_TRUE(joined.find(R"("name":"","count":298)") != std::string::npos);
    std::size_t names = 0;
    for (std::size_t at = joined.find("\"name\":"); at != std::string::npos; at = joined.find("\"name\":", at + 1)) {
        ++names;
    }
    EXPECT_EQ(names, std::size_t{301});
    EXPECT_TRUE(!find_op(texts, "entity_done").empty());

    // -- open replays exactly the block's widgets, with their values --
    client.send_text_frame(R"({"op":"open","entity":"0x70b3d5edc2000042","group":"Input 1"})");
    texts = collect(h, client, 5);
    std::size_t specs = 0;
    for (auto const& t : texts) {
        auto const msg = json_scan(t);
        if (msg.has_value() && msg->string_of("op") == "widget") {
            ++specs;
        }
    }
    EXPECT_EQ(specs, std::size_t{5});
    auto const gain = json_scan(find_op(texts, "widget", "\"Gain\""));
    EXPECT_TRUE(gain.has_value());
    if (gain.has_value()) {
        EXPECT_TRUE(gain->string_of("group") == "Input 1");
    }
    EXPECT_TRUE(!find_op(texts, "value", "\"control\":0").empty());
    EXPECT_TRUE(!find_op(texts, "entity_done").empty());

    // -- an unsolicited change in the open group reaches the client --
    std::array<uint8_t, 5> const enable_on{0x00, 0x1A, 0x00, 0x02, 0x01};
    h.fake->sink().on_unsolicited(DEVICE, AEM_COMMAND_SET_CONTROL, AEM_STATUS_SUCCESS, enable_on);
    texts = collect(h, client, 3);
    EXPECT_TRUE(!find_op(texts, "value", "\"control\":2").empty());

    // -- after close, the same push is filtered out --
    client.send_text_frame(R"({"op":"close","entity":"0x70b3d5edc2000042","group":"Input 1"})");
    (void)collect(h, client, 2);
    std::array<uint8_t, 5> const enable_off{0x00, 0x1A, 0x00, 0x02, 0x00};
    h.fake->sink().on_unsolicited(DEVICE, AEM_COMMAND_SET_CONTROL, AEM_STATUS_SUCCESS, enable_off);
    texts = collect(h, client, 3);
    EXPECT_TRUE(find_op(texts, "value", "\"control\":2").empty());

    // -- opening a group on a non-jumbo entity id is an error, not a hang --
    client.send_text_frame(R"({"op":"open","entity":"0x0000000000000001","group":"Input 1"})");
    texts = collect(h, client, 3);
    EXPECT_TRUE(!find_op(texts, "error", "non-jumbo").empty());
}

TEST_MAIN(statusbar_widget_bridge, widget_bridge_test)
