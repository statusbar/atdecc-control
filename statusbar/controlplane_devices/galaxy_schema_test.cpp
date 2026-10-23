// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the generated GALAXY 816 namespace: full-namespace
// resolution (the M3 acceptance criterion), spot-checked defaults from the
// programming guide's tables, and the describe-response format.

#include "statusbar/controlplane_devices/galaxy_schema.hpp"

#include "statusbar/test/test.hpp"

#include <string>

using namespace statusbar;
using namespace statusbar::controlplane;
using namespace statusbar::controlplane_devices;

namespace {

GalaxyNamespace const& ns()
{
    static GalaxyNamespace instance = build_galaxy_816_namespace();
    return instance;
}

std::string default_of(char const* path)
{
    auto h = ns().adaptor.resolve(path);
    EXPECT_TRUE(h.has_value());
    return h ? ns().defaults[*h] : std::string("<unresolved>");
}

}  // namespace

TEST(galaxy_schema, full_namespace_resolves)
{
    auto const& n = ns();
    EXPECT_TRUE(n.size() > 6000u);
    EXPECT_EQ(n.model->size(), n.size());
    for (ControlPointId h = 0; h < n.size(); ++h) {
        auto const& path = n.adaptor.address_of(h);
        EXPECT_FALSE(path.empty());
        auto back = n.adaptor.resolve(path);
        EXPECT_TRUE(back.has_value());
        EXPECT_EQ(*back, h);
    }
}

TEST(galaxy_schema, guide_table_defaults)
{
    EXPECT_TRUE(default_of("/processing/input/1/mute") == "true");
    EXPECT_TRUE(default_of("/processing/output/16/mute") == "false");
    EXPECT_TRUE(default_of("/processing/input/8/eq/3/frequency") == "500");
    EXPECT_TRUE(default_of("/processing/output/9/eq/10/frequency") == "16000");
    EXPECT_TRUE(default_of("/processing/output/1/atmospheric/gain") == "10");
    // U-Shaping band 5 is gain-only.
    EXPECT_TRUE(ns().adaptor.resolve("/processing/input/1/ushaping/5/gain").has_value());
    EXPECT_FALSE(ns().adaptor.resolve("/processing/input/1/ushaping/5/frequency").has_value());
    EXPECT_FALSE(ns().adaptor.resolve("/processing/input/1/eq/6/gain").has_value());  // input EQ has 5 bands
    // Matrix crosspoint gain default asymmetry.
    EXPECT_TRUE(default_of("/processing/matrix/1/1/gain") == "0");
    EXPECT_TRUE(default_of("/processing/matrix/1/8/gain") == "0");
    EXPECT_TRUE(default_of("/processing/matrix/1/9/gain") == "-90");
    EXPECT_TRUE(default_of("/processing/matrix/2/9/gain") == "0");
    EXPECT_TRUE(default_of("/processing/matrix/2/1/gain") == "-90");
    EXPECT_TRUE(default_of("/processing/matrix/32/16/gain") == "-90");
    // /status is telemetry, never folded: the guide's template values
    // stand (inputs 1-2 '3', 3-32 '4'), not the dumped unit's clock locks.
    EXPECT_TRUE(default_of("/status/clock/input/2/sync") == "3");
    EXPECT_TRUE(default_of("/status/clock/input/3/sync") == "4");
    // Per-instance name defaults.
    EXPECT_TRUE(default_of("/device/input/1/name") == "Input A");
    EXPECT_TRUE(default_of("/device/input/8/name") == "Input H");
    EXPECT_TRUE(default_of("/device/input/9/name") == "Matrix In 9");
    EXPECT_TRUE(default_of("/device/output/16/name") == "Output 16");
    // Snapshot 0 is special; the rest are generic.
    EXPECT_TRUE(default_of("/project/snapshot/0/locked") == "true");
    EXPECT_TRUE(default_of("/project/snapshot/0/name") == "Factory Defaults");
    EXPECT_TRUE(default_of("/project/snapshot/255/locked") == "false");
    EXPECT_TRUE(default_of("/project/snapshot/active/id") == "-1");
    // The document-artifact row is excluded.
    EXPECT_FALSE(ns().adaptor.resolve("/processing/matrix/Matrix").has_value());
}

TEST(galaxy_schema, live_dump_defaults_folded)
{
    // Firmware 2.10 factory values from the dump override stale guide
    // tables: U-Shaping bands doubled, per-band filter ladders replace the
    // template's flat placeholder.
    EXPECT_TRUE(default_of("/processing/input/1/ushaping/1/frequency") == "125");
    EXPECT_TRUE(default_of("/processing/output/16/ushaping/4/frequency") == "8000");
    EXPECT_TRUE(default_of("/processing/input/1/ushaping/1/slope") == "1");
    EXPECT_TRUE(default_of("/processing/output/1/delay_integration/1/frequency") == "32");
    EXPECT_TRUE(default_of("/processing/output/1/delay_integration/14/frequency") == "16000");
    EXPECT_TRUE(default_of("/processing/output/1/allpass/3/frequency") == "128");
    // The model's numeric def follows the folded string.
    auto us1 = *ns().adaptor.resolve("/processing/input/1/ushaping/1/frequency");
    EXPECT_EQ(ns().model->def(us1), 125.0);
    // /entity is per-unit identity, never folded: the guide's representative
    // values stand (empty serial number), not the dumped unit's.
    EXPECT_TRUE(default_of("/entity/serial_number") == "");
    EXPECT_TRUE(default_of("/entity/entity_name") == "GALAXY-18139889");
    EXPECT_TRUE(default_of("/entity/firmware_version") == "2.1.0-R4-1907032112");
    EXPECT_TRUE(default_of("/status/network/1/ip_address") == "169.254.7.39");
    EXPECT_TRUE(default_of("/status/avb/input/9/connected_talker/entity_name") == "");
    // Curated LIVE_DRIFT keeps the guide default despite the dump: the
    // in-use unit's unmuted inputs and moved gains are not defaults.
    EXPECT_TRUE(default_of("/processing/input/1/mute") == "true");
    EXPECT_TRUE(default_of("/processing/input/8/gain") == "0");
    EXPECT_TRUE(default_of("/system/access/8/privilege") == "9223372036854775807");
}

TEST(galaxy_schema, live_ranges_folded_into_model)
{
    auto const& n = ns();
    auto const& m = *n.model;
    auto gain = *n.adaptor.resolve("/processing/input/1/gain");
    EXPECT_EQ(m.min(gain), -90.0);
    EXPECT_EQ(m.max(gain), 10.0);
    EXPECT_EQ(m.step(gain), 0.1);
    EXPECT_EQ(m.units(gain), 0xB0u);  // dB
    // Delays are in samples on this device (48000 = 0.5 s at 96 kHz).
    auto delay = *n.adaptor.resolve("/processing/input/1/delay");
    EXPECT_EQ(m.max(delay), 48000.0);
    EXPECT_EQ(m.units(delay), 0x0Eu);  // samples
    // U-Shaping frequency ranges are per band.
    auto us1 = *n.adaptor.resolve("/processing/input/1/ushaping/1/frequency");
    auto us4 = *n.adaptor.resolve("/processing/input/1/ushaping/4/frequency");
    EXPECT_EQ(m.max(us1), 2500.0);
    EXPECT_EQ(m.max(us4), 20000.0);
    // Unranged points (bools, strings) stay min == max == 0.
    auto mute = *n.adaptor.resolve("/processing/input/1/mute");
    EXPECT_EQ(m.min(mute), m.max(mute));
    // The describe response now carries the live metadata.
    auto response = galaxy_describe_response(n, gain);
    EXPECT_TRUE(response.find("'minimum' : '-90'") != std::string::npos);
    EXPECT_TRUE(response.find("'maximum' : '10'") != std::string::npos);
    EXPECT_TRUE(response.find("'step' : '0.1'") != std::string::npos);
    EXPECT_TRUE(response.find("'units' : 'dB'") != std::string::npos);
}

TEST(galaxy_schema, enum_labels_folded)
{
    auto const& n = ns();
    auto veq = *n.adaptor.resolve("/processing/input/1/variable_eq/type");
    auto labels = n.enum_labels(veq);
    EXPECT_EQ(labels.size(), 4u);
    EXPECT_TRUE(std::string_view(labels[0]) == "0:NOTCH");
    EXPECT_TRUE(std::string_view(labels[1]) == "1:BANDPASS");
    // The delay-integration filter type carries the full 162-entry table.
    auto di = *n.adaptor.resolve("/processing/output/3/delay_integration/type");
    EXPECT_EQ(n.enum_labels(di).size(), 162u);
    // Non-enumerated points have none.
    auto gain = *n.adaptor.resolve("/processing/input/1/gain");
    EXPECT_EQ(n.enum_labels(gain).size(), 0u);
    // The describe response carries the list in the device's own format.
    auto response = galaxy_describe_response(n, veq);
    EXPECT_TRUE(response.find("'enum' : ['0:NOTCH', '1:BANDPASS', '2:ALLPASS'") != std::string::npos);
    EXPECT_TRUE(galaxy_describe_response(n, gain).find("'enum' : []") != std::string::npos);
}

TEST(galaxy_schema, alias_numbers_folded)
{
    auto const& n = ns();
    // Live-captured device aliases: the item indices of the Q1's own tree.
    auto mute = *n.adaptor.resolve("/processing/input/1/mute");
    auto gain = *n.adaptor.resolve("/processing/input/1/gain");
    auto mode = *n.adaptor.resolve("/device/input/1/mode");
    EXPECT_EQ(n.alias_of(mute), 370u);
    EXPECT_EQ(n.alias_of(gain), 371u);
    EXPECT_EQ(n.alias_of(mode), 15u);
    // alias -> handle is the exact inverse over the whole namespace.
    EXPECT_EQ(n.alias_handles.size(), n.size());
    for (std::size_t h = 0; h < n.size(); ++h) {
        EXPECT_EQ(n.handle_of_alias(n.alias_of(ControlPointId(h))), h);
    }
    // The describe response carries it in the device's format.
    auto response = galaxy_describe_response(n, gain);
    EXPECT_TRUE(response.find("'alias' : '@371'") != std::string::npos);
}

TEST(galaxy_schema, alias_addressing)
{
    auto const& n = ns();
    auto gain = *n.adaptor.resolve("/processing/input/1/gain");
    // Both address forms resolve to the same handle.
    EXPECT_EQ(*n.resolve_address("/processing/input/1/gain"), gain);
    EXPECT_EQ(*n.resolve_address("@371"), gain);
    // Malformed or out-of-range aliases resolve to nothing.
    EXPECT_FALSE(n.resolve_address("@").has_value());
    EXPECT_FALSE(n.resolve_address("@abc").has_value());
    EXPECT_FALSE(n.resolve_address("@9766").has_value());
    EXPECT_FALSE(n.resolve_address("@9999999999").has_value());
    // Selection: @N is a one-point selection, everything else a pattern.
    auto one = n.resolve_selection("@371");
    EXPECT_EQ(one.count(), 1u);
    EXPECT_TRUE(one.test(gain));
    EXPECT_EQ(n.resolve_selection("@9766").count(), 0u);
    EXPECT_EQ(n.resolve_selection("/processing/input/#/gain").count(), 8u);
}

TEST(galaxy_schema, read_only_and_model_metadata)
{
    auto const& n = ns();
    auto status = n.adaptor.resolve_pattern("/status/meter/input/#");
    EXPECT_EQ(status.count(), 8u);
    status.for_each_set([&](ControlPointId h) { EXPECT_TRUE(n.model->read_only(h)); });

    auto gain = n.adaptor.resolve("/processing/output/1/atmospheric/gain");
    EXPECT_TRUE(gain.has_value());
    EXPECT_FALSE(n.model->read_only(*gain));
    EXPECT_EQ(n.model->def(*gain), 10.0);
    // Descriptions are the device's verbatim names, not path-derived.
    EXPECT_TRUE(n.descriptions[*gain] == "Output 1 Gain");

    auto mute = n.adaptor.resolve("/processing/input/1/mute");
    EXPECT_TRUE(n.descriptions[*mute] == "Input 1 Mute");

    auto asrc = *n.adaptor.resolve("/device/input/1/aes/enable_asrc");
    EXPECT_TRUE(n.descriptions[asrc] == "Input 1 AES Receiver SRC Enabled");
    // The firmware's empty-prefix leading space survives verbatim.
    auto entity_name = *n.adaptor.resolve("/entity/entity_name");
    EXPECT_TRUE(n.descriptions[entity_name] == " Entity Name");
}

TEST(galaxy_schema, describe_response_shape)
{
    auto const& n = ns();
    auto mute = *n.adaptor.resolve("/processing/input/1/mute");
    auto response = galaxy_describe_response(n, mute);
    EXPECT_TRUE(
        response ==
        "?{'/processing/input/1/mute': { 'description' : 'Input 1 Mute', 'read_only' : 'false', "
        "'name' : 'Input 1 Mute', 'value' : 'true', 'minimum' : '', 'maximum' : '', 'default' : 'true', "
        "'step' : '', 'units' : '', 'enum' : [], 'alias' : '@370'}}");

    // The one device name containing an apostrophe stays parseable in both
    // literal grammars via escaping.
    auto asrc = *n.adaptor.resolve("/device/output/aes/enable_asrc");
    auto asrc_response = galaxy_describe_response(n, asrc);
    EXPECT_TRUE(asrc_response.find("'Output AES Transmitter SRC\\'s Enabled'") != std::string::npos);

    // Multi-point responses are one dict; entries are comma-separated.
    auto sel = n.adaptor.resolve_pattern("/device/sim/probe/#/point");
    EXPECT_EQ(sel.count(), 2u);
    auto multi = galaxy_describe_response(n, sel);
    EXPECT_TRUE(multi.find("'/device/sim/probe/1/point'") != std::string::npos);
    EXPECT_TRUE(multi.find("}, '/device/sim/probe/2/point'") != std::string::npos);

    // Values containing quotes stay parseable: log_message embeds double
    // quotes, which need no escaping inside single quotes.
    auto log = *n.adaptor.resolve("/status/log_message");
    auto log_response = galaxy_describe_response(n, log);
    EXPECT_TRUE(log_response.find("TIME=\"2019-08-08T05:21:09\"") != std::string::npos);
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane_devices, galaxy_schema_test)
