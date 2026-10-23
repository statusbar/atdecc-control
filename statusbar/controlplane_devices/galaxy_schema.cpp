// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane_devices/galaxy_schema.hpp"

#include <cassert>
#include <charconv>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace statusbar::controlplane_devices {

using controlplane::ControlPointId;
using controlplane::Selector;

namespace {

// ---- template expansion -------------------------------------------------
//
// Patterns carry {lo-hi} numeric ranges and {a|b|c} alternations; every
// expansion value is captured as a group. Defaults may reference groups:
// "$1" is group 1's text, "$@1" maps a numeric group to a letter (1 -> A),
// so "/device/input/{1-8}/name" defaults to "Input $@1" = 'Input A'..'H'.
// Templates expand in table order and the first mapping of a path wins,
// which is how specific defaults (matrix crosspoint gain, clock input
// sync) override the general template that follows them.

struct Emit
{
    std::vector<GalaxyPoint>& out;
    std::unordered_set<std::string> seen;

    void operator()(std::string path, std::string def, bool read_only)
    {
        if (seen.insert(path).second) {
            out.push_back({std::move(path), std::move(def), read_only});
        }
    }
};

std::string substitute(std::string_view def, std::vector<std::string> const& groups)
{
    std::string out;
    for (std::size_t i = 0; i < def.size(); ++i) {
        if (def[i] != '$' || i + 1 >= def.size()) {
            out.push_back(def[i]);
            continue;
        }
        bool letter = def[i + 1] == '@';
        std::size_t digit_at = letter ? i + 2 : i + 1;
        if (digit_at >= def.size() || def[digit_at] < '1' || def[digit_at] > '9') {
            out.push_back(def[i]);
            continue;
        }
        auto const& g = groups[std::size_t(def[digit_at] - '1')];
        if (letter) {
            unsigned n = 0;
            std::from_chars(g.data(), g.data() + g.size(), n);
            out.push_back(char('A' + n - 1));
        } else {
            out.append(g);
        }
        i = digit_at;
    }
    return out;
}

void expand(
    std::string_view pattern,
    std::string_view def,
    bool read_only,
    std::string& prefix,
    std::vector<std::string>& groups,
    Emit& emit)
{
    auto open = pattern.find('{');
    if (open == std::string_view::npos) {
        auto saved = prefix.size();
        prefix.append(pattern);
        emit(prefix, substitute(def, groups), read_only);
        prefix.resize(saved);
        return;
    }
    auto close = pattern.find('}', open);
    assert(close != std::string_view::npos);
    auto saved = prefix.size();
    prefix.append(pattern.substr(0, open));
    auto body = pattern.substr(open + 1, close - open - 1);
    auto rest = pattern.substr(close + 1);

    auto with_value = [&](std::string value) {
        auto inner = prefix.size();
        prefix.append(value);
        groups.push_back(std::move(value));
        expand(rest, def, read_only, prefix, groups, emit);
        groups.pop_back();
        prefix.resize(inner);
    };

    auto dash = body.find('-');
    if (body.find('|') == std::string_view::npos && dash != std::string_view::npos) {
        unsigned lo = 0, hi = 0;
        std::from_chars(body.data(), body.data() + dash, lo);
        std::from_chars(body.data() + dash + 1, body.data() + body.size(), hi);
        for (unsigned v = lo; v <= hi; ++v) {
            with_value(std::to_string(v));
        }
    } else {
        std::size_t i = 0;
        while (i <= body.size()) {
            auto bar = body.find('|', i);
            if (bar == std::string_view::npos) {
                bar = body.size();
            }
            with_value(std::string(body.substr(i, bar - i)));
            i = bar + 1;
        }
    }
    prefix.resize(saved);
}

struct Builder
{
    std::vector<GalaxyPoint> points;
    Emit emit{points};

    void add(std::string_view pattern, std::string_view def, bool read_only = false)
    {
        std::string prefix;
        std::vector<std::string> groups;
        expand(pattern, def, read_only, prefix, groups, emit);
    }
};

// The talker/listener status block is shared between /status/avb/clock/1
// and /status/avb/input/1-32 (Tables 19 and 20; identical shapes).
void add_avb_status_block(Builder& b, std::string_view base)
{
    auto add = [&](std::string_view tail, std::string_view def) { b.add(std::string(base) + std::string(tail), def, true); };
    for (std::string_view role : {"/connected_talker", "/redundant_connected_talker"}) {
        std::string r(role);
        add(r + "/channel_index", "-1");
        add(r + "/channel_name", "");
        add(r + "/connection_error", "");
        add(r + "/entity_id", "0x0");
        add(r + "/entity_name", "");
        add(r + "/group_name", "");
        add(r + "/presentation_time", "0");
        add(r + "/sample_rate", "0");
        add(r + "/stream_format", "0x0");
        add(r + "/stream_index", "-1");
        add(r + "/stream_name", "");
    }
    // The guide hyphenates "redundant-listener" in both editions; the
    // device serves the underscore form (verified live, Q1 fw 2.10.0).
    for (auto [role, stream_index] :
         {std::pair<std::string_view, std::string_view>{"/listener", "8"}, {"/redundant_listener", "17"}}) {
        std::string r(role);
        add(r + "/channel_index", "-1");
        add(r + "/channel_name", "");
        add(r + "/early_timestamp_count", "0");
        add(r + "/late_timestamp_count", "0");
        add(r + "/media_locked", "false");
        add(r + "/media_reset_count", "0");
        add(r + "/msrp_accumulated_latency", "0");
        add(r + "/presentation_time_margin", "0");
        add(r + "/sequence_mismatch_count", "0");
        add(r + "/stream_format", "0x41060010000bb80");
        add(r + "/stream_index", stream_index);
        add(r + "/stream_name", "Clock Sink 1");
        add(r + "/stream_reset_count", "0");
        add(r + "/timestamp_uncertain_count", "0");
        add(r + "/unsupported_format_count", "0");
    }
}

}  // namespace

std::vector<GalaxyPoint> galaxy_816_points()
{
    Builder b;

    // ---- /processing/input (1-8) ------------------------------------
    b.add("/processing/input/{1-8}/delay", "0");
    b.add("/processing/input/{1-8}/delay_type", "0");
    b.add("/processing/input/{1-8}/eq/1/frequency", "32");
    b.add("/processing/input/{1-8}/eq/2/frequency", "125");
    b.add("/processing/input/{1-8}/eq/3/frequency", "500");
    b.add("/processing/input/{1-8}/eq/4/frequency", "2000");
    b.add("/processing/input/{1-8}/eq/5/frequency", "8000");
    b.add("/processing/input/{1-8}/eq/{1-5}/band_bypass", "false");
    b.add("/processing/input/{1-8}/eq/{1-5}/bandwidth", "1");
    b.add("/processing/input/{1-8}/eq/{1-5}/gain", "0");
    b.add("/processing/input/{1-8}/eq/bypass", "false");
    b.add("/processing/input/{1-8}/equalization_bypass", "false");
    b.add("/processing/input/{1-8}/gain", "0");
    b.add("/processing/input/{1-8}/mute", "true");
    b.add("/processing/input/{1-8}/ushaping/1/frequency", "62");
    b.add("/processing/input/{1-8}/ushaping/2/frequency", "250");
    b.add("/processing/input/{1-8}/ushaping/3/frequency", "1000");
    b.add("/processing/input/{1-8}/ushaping/4/frequency", "4000");
    b.add("/processing/input/{1-8}/ushaping/{1-4}/slope", "2");
    b.add("/processing/input/{1-8}/ushaping/{1-5}/gain", "0");  // band 5 is gain-only
    b.add("/processing/input/{1-8}/ushaping/bypass", "false");
    // Firmware-only input points (absent from both guide editions; paths,
    // defaults, and ranges verified live on Q1 fw 2.10.0, 2026-08-29).
    b.add("/processing/input/{1-8}/odelay", "-1");
    b.add("/processing/input/{1-8}/ogain", "-100");
    b.add("/processing/input/{1-8}/solo", "false");
    b.add("/processing/input/{1-8}/variable_eq/bypass", "true");
    b.add("/processing/input/{1-8}/variable_eq/frequency", "1000");
    b.add("/processing/input/{1-8}/variable_eq/q", "1");
    b.add("/processing/input/{1-8}/variable_eq/type", "0");

    // ---- /processing/output (1-16) ----------------------------------
    b.add("/processing/output/{1-16}/atmospheric/bypass", "true");
    b.add("/processing/output/{1-16}/atmospheric/distance", "0");
    b.add("/processing/output/{1-16}/atmospheric/gain", "10");
    b.add("/processing/output/{1-16}/delay", "0");
    b.add("/processing/output/{1-16}/delay_type", "0");
    b.add("/processing/output/{1-16}/eq/1/frequency", "32");
    b.add("/processing/output/{1-16}/eq/2/frequency", "63");
    b.add("/processing/output/{1-16}/eq/3/frequency", "125");
    b.add("/processing/output/{1-16}/eq/4/frequency", "250");
    b.add("/processing/output/{1-16}/eq/5/frequency", "500");
    b.add("/processing/output/{1-16}/eq/6/frequency", "1000");
    b.add("/processing/output/{1-16}/eq/7/frequency", "2000");
    b.add("/processing/output/{1-16}/eq/8/frequency", "4000");
    b.add("/processing/output/{1-16}/eq/9/frequency", "8000");
    b.add("/processing/output/{1-16}/eq/10/frequency", "16000");
    b.add("/processing/output/{1-16}/eq/{1-10}/band_bypass", "false");
    b.add("/processing/output/{1-16}/eq/{1-10}/bandwidth", "1");
    b.add("/processing/output/{1-16}/eq/{1-10}/gain", "0");
    b.add("/processing/output/{1-16}/eq/bypass", "false");
    b.add("/processing/output/{1-16}/equalization_bypass", "false");
    b.add("/processing/output/{1-16}/gain", "0");
    b.add("/processing/output/{1-16}/highpass/bypass", "true");
    b.add("/processing/output/{1-16}/highpass/frequency", "40");
    b.add("/processing/output/{1-16}/highpass/type", "11");
    b.add("/processing/output/{1-16}/lowpass/bypass", "true");
    b.add("/processing/output/{1-16}/lowpass/frequency", "160");
    b.add("/processing/output/{1-16}/lowpass/type", "11");
    b.add("/processing/output/{1-16}/mute", "false");
    b.add("/processing/output/{1-16}/polarity_reversal", "false");
    b.add("/processing/output/{1-16}/ushaping/1/frequency", "62");
    b.add("/processing/output/{1-16}/ushaping/2/frequency", "250");
    b.add("/processing/output/{1-16}/ushaping/3/frequency", "1000");
    b.add("/processing/output/{1-16}/ushaping/4/frequency", "4000");
    b.add("/processing/output/{1-16}/ushaping/{1-4}/slope", "2");
    b.add("/processing/output/{1-16}/ushaping/{1-5}/gain", "0");
    b.add("/processing/output/{1-16}/ushaping/bypass", "false");
    // Firmware-only output points (verified live, Q1 fw 2.10.0): the
    // allpass and delay-integration filter banks, beam-control allpass,
    // solo/ogain/odelay, unlinked mute, and the variable EQ.
    b.add("/processing/output/{1-16}/allpass/{1-3}/band_bypass", "true");
    b.add("/processing/output/{1-16}/allpass/{1-3}/frequency", "20");
    b.add("/processing/output/{1-16}/allpass/{1-3}/q", "1");
    b.add("/processing/output/{1-16}/allpass/bypass", "true");
    b.add("/processing/output/{1-16}/beam_control_allpass/band_bypass", "true");
    b.add("/processing/output/{1-16}/beam_control_allpass/frequency", "20");
    b.add("/processing/output/{1-16}/beam_control_allpass/q", "1");
    b.add("/processing/output/{1-16}/delay_integration/{1-14}/band_bypass", "true");
    b.add("/processing/output/{1-16}/delay_integration/{1-14}/frequency", "20");
    b.add("/processing/output/{1-16}/delay_integration/{1-14}/q", "1");
    b.add("/processing/output/{1-16}/delay_integration/bypass", "false");
    b.add("/processing/output/{1-16}/delay_integration/polarity_reversal", "false");
    b.add("/processing/output/{1-16}/delay_integration/type", "1");
    b.add("/processing/output/{1-16}/odelay", "-1");
    b.add("/processing/output/{1-16}/ogain", "-100");
    b.add("/processing/output/{1-16}/solo", "false");
    b.add("/processing/output/{1-16}/unlinked_mute", "false");
    b.add("/processing/output/{1-16}/variable_eq/bypass", "true");
    b.add("/processing/output/{1-16}/variable_eq/frequency", "1000");
    b.add("/processing/output/{1-16}/variable_eq/q", "1");
    b.add("/processing/output/{1-16}/variable_eq/type", "0");

    // ---- /processing/matrix (input 1-32 x crosspoint 1-16) ----------
    // Only matrix input 1 crosspoints 1-8 and input 2 crosspoints 9-16
    // default to 0 dB; every other crosspoint gain defaults to -90 (first
    // mapping wins over the general template below).
    b.add("/processing/matrix/1/{1-8}/gain", "0");
    b.add("/processing/matrix/2/{9-16}/gain", "0");
    b.add("/processing/matrix/{1-32}/{1-16}/delay", "0");
    b.add("/processing/matrix/{1-32}/{1-16}/delay_bypass", "false");
    b.add("/processing/matrix/{1-32}/{1-16}/delay_type", "0");
    b.add("/processing/matrix/{1-32}/{1-16}/gain", "-90");
    // Firmware-only per-crosspoint trims (verified live, Q1 fw 2.10.0).
    b.add("/processing/matrix/{1-32}/{1-16}/trim", "0");
    b.add("/processing/matrix/{1-32}/{1-16}/delay_trim", "0");

    // ---- /processing/beam_control_array (firmware-only; verified live,
    // Q1 fw 2.10.0 — four arrays for beam-steered products) -----------
    b.add("/processing/beam_control_array/{1-4}/beam_angle", "20");
    b.add("/processing/beam_control_array/{1-4}/bypass", "true");
    b.add("/processing/beam_control_array/{1-4}/control_type", "0");
    b.add("/processing/beam_control_array/{1-4}/elements_per_output", "1");
    b.add("/processing/beam_control_array/{1-4}/error_code", "1", true);
    b.add("/processing/beam_control_array/{1-4}/error_string", "", true);
    b.add("/processing/beam_control_array/{1-4}/name", "Array $1");
    b.add("/processing/beam_control_array/{1-4}/number_of_elements", "12");
    b.add("/processing/beam_control_array/{1-4}/product_type", "0");
    b.add("/processing/beam_control_array/{1-4}/starting_element", "1");
    b.add("/processing/beam_control_array/{1-4}/starting_output_number", "1");
    b.add("/processing/beam_control_array/{1-4}/upper_freq_boundary", "400");

    // ---- /system ----------------------------------------------------
    b.add("/system/firmware/code", "0");
    b.add("/system/firmware/status", "0");
    b.add("/system/firmware/status_string", "");
    b.add("/system/hardware/front_panel_lockout", "false");
    b.add("/system/meter/demo/active", "false");
    b.add("/system/mode/running", "true");
    b.add("/system/network/1/static/ip_address", "192.168.0.2");
    b.add("/system/network/2/static/ip_address", "192.168.0.3");
    b.add("/system/network/{1-2}/static/gateway", "192.168.0.1");
    b.add("/system/network/{1-2}/static/net_mask", "255.255.255.0");
    b.add("/system/network/{1-2}/type", "0");
    // Firmware-only system points (verified live, Q1 fw 2.10.0).
    b.add("/system/access/{1-8}/code", "");
    b.add("/system/access/{1-8}/name", "Default");
    b.add("/system/access/{1-8}/privilege", "9223372036854775807");
    b.add("/system/batch", "0");
    b.add("/system/diagnostic/glitch_test/never_clear_meters", "false");
    b.add("/system/mode/core", "false", true);

    // ---- /device ----------------------------------------------------
    b.add("/device/preferences/brightness", "1");
    b.add("/device/preferences/display_color", "3");
    b.add("/device/sim/bus_address", "10");
    b.add("/device/sim/configured", "false");
    b.add("/device/sim/mute_relay/{1-4}", "true");
    b.add("/device/sim/probe/1/point", "2");
    b.add("/device/sim/probe/2/point", "4");
    b.add("/device/sim/probe/{1-2}/channel", "1");
    // Processed inputs 1-8; name defaults are correspondingly A-H.
    b.add("/device/input/{1-8}/aes/enable_asrc", "true");
    b.add("/device/input/{1-8}/input_link_group", "0");
    b.add("/device/input/{1-8}/isolate", "false");
    b.add("/device/input/{1-8}/mode", "1");
    b.add("/device/input/{1-8}/name", "Input $@1");
    b.add("/device/input/{1-8}/scale", "26");
    b.add("/device/input/{1-8}/select", "false");
    // Matrix inputs 9-32.
    b.add("/device/input/{9-32}/input_link_group", "0");
    b.add("/device/input/{9-32}/mode", "4");
    b.add("/device/input/{9-32}/name", "Matrix In $1");
    b.add("/device/input/{9-32}/select", "false");
    b.add("/device/input/avb/controller_mode", "0");
    b.add("/device/input_link_group/{1-4}/bypass", "true");
    b.add("/device/input_link_group/{1-4}/name", "Group $1");
    b.add("/device/output/{1-16}/isolate", "false");
    b.add("/device/output/{1-16}/mute_relay", "false");
    b.add("/device/output/{1-16}/name", "Output $1");
    b.add("/device/output/{1-16}/output_link_group", "0");
    b.add("/device/output/{1-16}/scale", "26");
    b.add("/device/output/{1-16}/select", "false");
    b.add("/device/output/{1-16}/sim/trim", "0");
    b.add("/device/output_link_group/{1-8}/bypass", "true");
    b.add("/device/output_link_group/{1-8}/name", "Group $1");
    b.add("/device/output/avb/presentation_time", "2000000");
    b.add("/device/output/atmospheric/altitude", "0");
    b.add("/device/output/atmospheric/humidity", "50");
    b.add("/device/output/atmospheric/temperature", "293.15");
    // Firmware-only device points (verified live, Q1 fw 2.10.0).
    b.add("/device/input/{1-32}/tags", "");
    b.add("/device/output/{1-16}/tags", "");
    b.add("/device/lcd_text_override/{1-2}", "");
    b.add("/device/output/aes/disable_high8ch_tx", "false");
    b.add("/device/output/aes/disable_low8ch_tx", "false");
    b.add("/device/output/aes/enable_asrc", "true");

    // ---- /project ---------------------------------------------------
    b.add("/project/boot_snapshot_id", "3");
    b.add("/project/metadata/content_type", "2");
    b.add("/project/metadata/schema_version", "10");
    b.add("/project/name", "Default");
    b.add("/project/project_firmware_version", "none");
    b.add("/project/snapshot/0/comment", "All settings are set to default values");
    b.add("/project/snapshot/0/created", "2018-11-05 22:58:07");
    b.add("/project/snapshot/0/last_updated", "2018-11-05 22:58:07");
    b.add("/project/snapshot/0/locked", "true");
    b.add("/project/snapshot/0/name", "Factory Defaults");
    b.add("/project/snapshot/{0-255}/comment", "");
    b.add("/project/snapshot/{0-255}/created", "");
    b.add("/project/snapshot/{0-255}/last_updated", "");
    b.add("/project/snapshot/{0-255}/locked", "false");
    b.add("/project/snapshot/{0-255}/modified", "false");
    b.add("/project/snapshot/{0-255}/name", "");
    b.add("/project/snapshot/active/comment", "");
    b.add("/project/snapshot/active/created", "2018-11-05 22:58:08");
    b.add("/project/snapshot/active/id", "-1");
    b.add("/project/snapshot/active/last_updated", "2018-11-05 22:58:08");
    b.add("/project/snapshot/active/locked", "true");
    b.add("/project/snapshot/active/modified", "true");
    b.add("/project/snapshot/active/name", "No snapshot selected");

    // ---- /entity (representative per-device values) -----------------
    b.add("/entity/entity_id", "0x1cabfffe008d80");
    b.add("/entity/entity_model_id", "0x1cabb804004005");
    b.add("/entity/entity_name", "GALAXY-18139889");
    b.add("/entity/firmware_version", "2.1.0-R4-1907032112");
    b.add("/entity/group_name", "GALAXYs");
    b.add("/entity/input_channel_count", "64");
    b.add("/entity/input_stream_count", "18");
    b.add("/entity/output_channel_count", "24");
    b.add("/entity/output_stream_count", "14");
    // No default: a serial number is per-unit identity; the mirror reads it
    // from the live device.
    b.add("/entity/serial_number", "");

    // ---- /status (all read-only) ------------------------------------
    b.add("/status/beam_control_input_source", "0", true);
    b.add("/status/clock/aes_output/input_number", "1", true);
    b.add("/status/clock/aes_output/sample_rate", "96000", true);
    b.add("/status/clock/aes_output/source", "0", true);
    b.add("/status/clock/aes_output/sync", "2", true);
    b.add("/status/clock/gptp/{1-2}/as_capable", "false", true);
    b.add("/status/clock/gptp/{1-2}/as_path/{1-18}/trace_id", "", true);
    b.add("/status/clock/gptp/{1-2}/as_path/trace_length", "0", true);
    b.add("/status/clock/gptp/{1-2}/grand_master_id", "00-1C-AB-FF-FE-00-8D-80", true);
    b.add("/status/clock/gptp/{1-2}/peer_delay", "0", true);
    b.add("/status/clock/gptp/{1-2}/port_locked", "true", true);
    b.add("/status/clock/gptp/{1-2}/port_role", "3", true);
    // Clock inputs 1-2 share one default set, 3-32 another.
    b.add("/status/clock/input/{1-2}/sync", "3", true);
    b.add("/status/clock/input/{1-32}/sample_rate", "0", true);
    b.add("/status/clock/input/{1-32}/sync", "4", true);
    b.add("/status/clock/rtc/date_and_time", "2019-08-08T05:23:11", true);
    b.add("/status/clock/system/input_number", "1", true);
    b.add("/status/clock/system/sample_rate", "96000", true);
    b.add("/status/clock/system/source", "0", true);
    b.add("/status/clock/system/sync", "2", true);
    b.add("/status/clock/word_clock/sample_rate", "0", true);
    b.add("/status/clock/word_clock/sync", "3", true);
    b.add("/status/clock/word_clock/termination", "0", true);
    b.add("/status/connected_client_count", "3", true);
    b.add("/status/connected_osc_tcp_client_count", "0", true);
    // The guide prints "upd" in both editions; the device serves "udp"
    // (verified live, Q1 fw 2.10.0).
    b.add("/status/connected_osc_udp_client_count", "0", true);
    b.add("/status/connected_text_tcp_client_count", "3", true);

    add_avb_status_block(b, "/status/avb/clock/1");
    for (unsigned i = 1; i <= 32; ++i) {
        add_avb_status_block(b, "/status/avb/input/" + std::to_string(i));
    }

    b.add("/status/hardware/board/digital/arm_temp", "299.15", true);
    b.add("/status/hardware/board/digital/fan/{1-4}/kickstarting", "false", true);
    b.add("/status/hardware/board/digital/fan/{1-4}/stalled", "false", true);
    b.add("/status/hardware/board/digital/fan/{1-4}/tach", "3750", true);
    b.add("/status/hardware/board/digital/fpga_aux_voltage", "1.78662", true);
    b.add("/status/hardware/board/digital/fpga_main_voltage", "0.995605", true);
    b.add("/status/hardware/board/digital/fpga_ram_voltage", "0.99707", true);
    b.add("/status/hardware/board/digital/fpga_ref_voltage", "1.25146", true);
    b.add("/status/hardware/board/digital/fpga_temp", "313.5", true);
    b.add("/status/hardware/board/digital/main_5v0_voltage", "5.10677", true);
    // The guide prints "vxco_pull" in both editions; the device serves
    // "vcxo_pull" (verified live, Q1 fw 2.10.0).
    b.add("/status/hardware/board/digital/vcxo_pull", "50", true);
    b.add("/status/hardware/board/digital/vcxo_sample_rate", "96001", true);
    // The guide shows primary_io and notes the same block exists for
    // secondary_io.
    b.add("/status/hardware/board/{primary_io|secondary_io}/analog_out_temp", "309.15", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/aes_out_src_present/{1-2}", "false", true);
    // The guide shows .../analog_in_eeprom_present/1; the device serves it
    // unindexed (verified live, Q1 fw 2.10.0).
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/analog_in_eeprom_present", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/analog_in_relay_present", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/analog_out_da_present/{1-4}", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/analog_out_eeprom_present", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/analog_out_mute_relay_present", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/analog_out_range_relay_present", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/analog_out_temp_sensor_present", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/line_mon_ad_present/{1-2}", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/i2c/sim_relay_present", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/trim/analog_in_ad/{1-4}/gain_comp", "0.386495", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/trim/analog_in_ad/{1-4}/range_valid", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/trim/analog_in_ad/crc_valid", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/trim/analog_out_da/{1-8}/gain_comp", "-0.121722", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/trim/analog_out_da/{1-8}/range_valid", "true", true);
    b.add("/status/hardware/board/{primary_io|secondary_io}/trim/analog_out_da/crc_valid", "true", true);
    b.add("/status/hardware/errors_exist", "false", true);

    b.add("/status/identify_active", "false", true);
    b.add("/status/lcd_log_message", "", true);
    b.add("/status/led/avb_sync", "0", true);
    b.add("/status/led/media_clock", "2", true);
    b.add("/status/led/network", "2", true);
    b.add("/status/led/sim_bus", "0", true);
    b.add(
        "/status/log_message",
        "TIME=\"2019-08-08T05:21:09\" ERR_MJR=0 ERR_MIN=0 PRI=6 STR=\"Compass connected: Compass v4.6.0\"",
        true);
    b.add("/status/matrix_crosspoints_used", "16", true);
    b.add("/status/meter/input/{1-8}", "-90", true);
    b.add("/status/meter/input_split/{1-8}", "0", true);
    // The guide shows line monitors 1-2; the device serves 1-16 (verified
    // live, Q1 fw 2.10.0).
    b.add("/status/meter/line_mon/{1-16}/{ideal|neg|pos}", "0", true);
    b.add("/status/meter/matrix_input/{1-32}", "-90", true);
    b.add("/status/meter/output/{1-16}", "-90", true);
    b.add("/status/meter/sim/{1-2}", "-90", true);
    b.add("/status/model_string", "GX-816", true);
    b.add("/status/network/{1-2}/carrier", "1", true);
    b.add("/status/network/{1-2}/duplex", "full", true);
    b.add("/status/network/{1-2}/gateway", "", true);
    b.add("/status/network/{1-2}/ip_address", "169.254.7.39", true);
    b.add("/status/network/{1-2}/mac_address", "00:1C:AB:00:8D:80", true);
    b.add("/status/network/{1-2}/net_mask", "255.255.0.0", true);
    b.add("/status/network/{1-2}/speed", "1000", true);
    b.add("/status/snapshot/recall_in_progress", "false", true);
    // Firmware-only talker-side stream status (verified live, Q1 fw
    // 2.10.0; the device serves six AVB outputs here).
    b.add("/status/avb/output/{1-6}/presentation_time", "2000000", true);
    b.add("/status/avb/output/{1-6}/redundant_presentation_time", "2000000", true);
    b.add("/status/avb/output/{1-6}/redundant_sample_rate", "0", true);
    b.add("/status/avb/output/{1-6}/redundant_stream_index", "-1", true);
    b.add("/status/avb/output/{1-6}/redundant_stream_name", "", true);
    b.add("/status/avb/output/{1-6}/sample_rate", "0", true);
    b.add("/status/avb/output/{1-6}/stream_index", "-1", true);
    b.add("/status/avb/output/{1-6}/stream_name", "", true);

    return std::move(b.points);
}

namespace {

#include "statusbar/controlplane_devices/galaxy_schema_ranges.inc"

/// The range table's shape key: numeric segments -> '#', except a band
/// number following "ushaping" (per-band ranges — see fold_describes.py).
std::string range_shape_of(std::string_view path)
{
    std::string out;
    std::string_view previous;
    controlplane::split_path(path, [&](std::string_view seg) {
        bool numeric = !seg.empty();
        for (char c : seg) {
            if (c < '0' || c > '9') {
                numeric = false;
                break;
            }
        }
        out += '/';
        if (numeric && previous != "ushaping") {
            out += '#';
        } else {
            out += seg;
        }
        previous = seg;
    });
    return out;
}

GalaxyEnum const* find_enum(std::string_view path)
{
    static auto const* by_shape = [] {
        auto* map = new std::unordered_map<std::string_view, GalaxyEnum const*>();
        for (auto const& e : GALAXY_ENUMS) {
            (*map)[e.shape] = &e;
        }
        return map;
    }();
    auto it = by_shape->find(range_shape_of(path));
    return it == by_shape->end() ? nullptr : it->second;
}

/// The alias-table entry for an exact path (every generated path has one).
GalaxyAliasEntry const* find_alias_entry(std::string_view path)
{
    static auto const* by_path = [] {
        auto* map = new std::unordered_map<std::string_view, GalaxyAliasEntry const*>();
        for (auto const& e : GALAXY_ALIASES) {
            (*map)[e.path] = &e;
        }
        return map;
    }();
    auto it = by_path->find(path);
    return it == by_path->end() ? nullptr : it->second;
}

GalaxyRange const* find_range(std::string_view path)
{
    static auto const* by_shape = [] {
        auto* map = new std::unordered_map<std::string_view, GalaxyRange const*>();
        for (auto const& r : GALAXY_RANGES) {
            (*map)[r.shape] = &r;
        }
        return map;
    }();
    auto it = by_shape->find(range_shape_of(path));
    return it == by_shape->end() ? nullptr : it->second;
}

/// The GALAXY's own units label for a folded AEM unit code (describe
/// responses speak the device's vocabulary).
std::string_view units_label(std::uint8_t code)
{
    switch (code) {
        case 0xB0:
            return "dB";
        case 0xB1:
            return "dB (Peak)";
        case 0x30:
            return "V";
        case 0x10:
            return "Hz";
        case 0x13:
            return "Octaves";
        case 0x0E:
            return "samples";
        case 0x08:
            return "sec";
        case 0x18:
            return "metres";
        case 0x02:
            return "%";
        case 0x20:
            return "ºK";
        default:
            return "";
    }
}

std::string format_number(double v)
{
    char buf[48];
    snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

/// Escapes a value for a single-quoted string acceptable to both the
/// Python literal grammar and a JavaScript expression.
void append_quoted(std::string& out, std::string_view value)
{
    out.push_back('\'');
    for (char c : value) {
        if (c == '\'' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('\'');
}

void append_describe_entry(std::string& out, GalaxyNamespace const& ns, ControlPointId h)
{
    auto const& path = ns.adaptor.address_of(h);
    append_quoted(out, path);
    out += ": { 'description' : ";
    append_quoted(out, ns.descriptions[h]);
    out += ", 'read_only' : ";
    append_quoted(out, ns.model->read_only(h) ? "true" : "false");
    // The device serves an identical human-readable string as both the
    // description and the name.
    out += ", 'name' : ";
    append_quoted(out, ns.descriptions[h]);
    out += ", 'value' : ";
    append_quoted(out, ns.defaults[h]);
    auto const& model = *ns.model;
    bool const ranged = model.min(h) < model.max(h);
    out += ", 'minimum' : ";
    append_quoted(out, ranged ? format_number(model.min(h)) : "");
    out += ", 'maximum' : ";
    append_quoted(out, ranged ? format_number(model.max(h)) : "");
    out += ", 'default' : ";
    append_quoted(out, ns.defaults[h]);
    out += ", 'step' : ";
    append_quoted(out, ranged ? format_number(model.step(h)) : "");
    out += ", 'units' : ";
    append_quoted(out, ranged ? std::string(units_label(std::uint8_t(model.units(h)))) : "");
    // The enum label list rides as a bare list of quoted strings — the
    // device's own format, still parseable in both literal grammars.
    out += ", 'enum' : [";
    bool first_label = true;
    for (auto const* label : ns.enum_labels(h)) {
        if (!first_label) {
            out += ", ";
        }
        first_label = false;
        append_quoted(out, label);
    }
    out += "], 'alias' : ";
    append_quoted(out, "@" + std::to_string(ns.alias_of(h)));
    out += "}";
}

}  // namespace

GalaxyNamespace build_galaxy_816_namespace()
{
    auto points = galaxy_816_points();

    GalaxyNamespace ns;
    controlplane::ModelBuilder model;
    controlplane::PathAdaptorBuilder paths(points.size());
    ns.defaults.reserve(points.size());
    ns.descriptions.reserve(points.size());

    for (auto const& p : points) {
        controlplane::ControlPointDesc d;
        if (p.read_only) {
            d.flags = controlplane::PointFlags::read_only;
        }
        // Defaults are the folded live-dump values (galaxy_schema_ranges.inc);
        // a null entry marks curated live drift, where the template's guide
        // default stands.
        auto const* alias_entry = find_alias_entry(p.path);
        std::string const default_value = alias_entry && alias_entry->def ? std::string(alias_entry->def) : p.default_value;
        // Numeric defaults land in the model's def field. Floating-point
        // from_chars needs a newer macOS deployment target, so validate the
        // shape by hand (plain decimal only — no hex, no inf/nan) and let
        // strtod do the conversion.
        auto const& s = default_value;
        bool numeric = !s.empty();
        for (char c : s) {
            if ((c < '0' || c > '9') && c != '-' && c != '+' && c != '.' && c != 'e' && c != 'E') {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            char* end = nullptr;
            double num = std::strtod(s.c_str(), &end);
            if (end == s.c_str() + s.size()) {
                d.def = num;
            }
        }
        // Ranges and units come from the live device's describe metadata
        // (galaxy_schema_ranges.inc, folded from captures/).
        if (auto const* range = find_range(p.path)) {
            d.min = range->min;
            d.max = range->max;
            d.step = range->step;
            d.multiplier = range->unit_multiplier;
            d.units = range->unit_code;
        }
        if (auto const* en = find_enum(p.path)) {
            ns.enums.push_back({en->labels, en->count});
        } else {
            ns.enums.push_back({nullptr, 0});
        }
        auto h = model.add(d);
        bool mapped = paths.map(p.path, h);
        assert(mapped);
        (void)mapped;
        ns.defaults.push_back(default_value);
        ns.descriptions.emplace_back();  // filled from the alias table below
    }

    ns.model = model.build();
    ns.adaptor = paths.build();

    // Bind the device's alias numbers (its item indices) to our handles:
    // GALAXY_ALIASES is the full namespace in alias order, so every path
    // must resolve and every handle must get exactly one alias — and its
    // verbatim human-readable name.
    assert(std::size(GALAXY_ALIASES) == points.size());
    ns.aliases.resize(points.size());
    ns.alias_handles.resize(points.size());
    for (std::size_t alias = 0; alias < std::size(GALAXY_ALIASES); ++alias) {
        auto h = ns.adaptor.resolve(GALAXY_ALIASES[alias].path);
        assert(h.has_value());
        ns.aliases[*h] = std::uint16_t(alias);
        ns.alias_handles[alias] = *h;
        ns.descriptions[*h] = GALAXY_ALIASES[alias].name;
    }
    return ns;
}

std::optional<ControlPointId> GalaxyNamespace::resolve_address(std::string_view address) const
{
    if (address.size() >= 2 && address.front() == '@') {
        std::uint32_t alias = 0;
        for (char c : address.substr(1)) {
            if (c < '0' || c > '9' || alias > alias_handles.size()) {
                return std::nullopt;
            }
            alias = alias * 10 + std::uint32_t(c - '0');
        }
        if (alias >= alias_handles.size()) {
            return std::nullopt;
        }
        return alias_handles[alias];
    }
    return adaptor.resolve(address);
}

Selector GalaxyNamespace::resolve_selection(std::string_view address_or_pattern) const
{
    if (!address_or_pattern.empty() && address_or_pattern.front() == '@') {
        Selector out(size());
        if (auto h = resolve_address(address_or_pattern)) {
            out.set(*h);
        }
        return out;
    }
    return adaptor.resolve_pattern(address_or_pattern);
}

std::string galaxy_describe_response(GalaxyNamespace const& ns, Selector const& points)
{
    std::string out = "?{";
    bool first = true;
    points.for_each_set([&](ControlPointId h) {
        if (!first) {
            out += ", ";
        }
        first = false;
        append_describe_entry(out, ns, h);
    });
    out += "}";
    return out;
}

std::string galaxy_describe_response(GalaxyNamespace const& ns, ControlPointId h)
{
    Selector one(ns.size());
    one.set(h);
    return galaxy_describe_response(ns, one);
}

}  // namespace statusbar::controlplane_devices
