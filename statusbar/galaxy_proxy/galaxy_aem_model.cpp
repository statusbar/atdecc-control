// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"

#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_aem_units.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"

#include <cassert>
#include <cstring>
#include <optional>

namespace statusbar::galaxy_proxy {

using namespace statusbar::atdecc::aem;
using controlplane::AemBindingBuilder;
using controlplane::AemRef;
using controlplane::ControlPointId;
using controlplane_devices::GalaxyNamespace;
using ieee::Eui64;

namespace {

// ---- provisional vendor control types (JDKS OUI-36, bank 0x1) -----------

constexpr Eui64 vendor_type(std::uint8_t n)
{
    return Eui64{0x70, 0xB3, 0xD5, 0xED, 0xC1, 0x00, 0x00, n};
}

Eui64 const VT_DELAY_DISPLAY_TYPE = vendor_type(0x01);
Eui64 const VT_EQ_BAND = vendor_type(0x02);
Eui64 const VT_USHAPING_BAND = vendor_type(0x03);
Eui64 const VT_USHAPING_BAND5_GAIN = vendor_type(0x04);
Eui64 const VT_LINK_GROUP_ASSIGN = vendor_type(0x05);
Eui64 const VT_ISOLATE = vendor_type(0x06);
Eui64 const VT_SELECT = vendor_type(0x07);
Eui64 const VT_MUTE_RELAY = vendor_type(0x08);
Eui64 const VT_DISPLAY_COLOR = vendor_type(0x09);
Eui64 const VT_FRONT_PANEL_LOCKOUT = vendor_type(0x0A);
Eui64 const VT_ATMOSPHERIC_DISTANCE = vendor_type(0x0B);
Eui64 const VT_CROSSPOINTS_USED = vendor_type(0x0C);
Eui64 const VT_BOARD_VOLTAGE = vendor_type(0x0D);
Eui64 const VT_LED_STATE = vendor_type(0x0E);
Eui64 const VT_LOG_MESSAGE = vendor_type(0x0F);
Eui64 const VT_FILTER_TYPE = vendor_type(0x10);
Eui64 const VT_SIM_TRIM = vendor_type(0x11);
Eui64 const VT_MATRIX_DELAY_TYPE = vendor_type(0x12);

constexpr std::uint16_t NO_LOCALIZED = 0xFFFF;
constexpr SignalSource NO_SIGNAL{0xFFFF, 0, 0};

// ---- unit shorthand -----------------------------------------------------

constexpr ControlUnits make_units(std::int8_t multiplier, std::uint8_t code)
{
    ControlUnits u;
    u.multiplier = ieee::octet_t(std::uint8_t(multiplier));
    u.code = ieee::octet_t(code);
    return u;
}

constexpr ControlUnits UNIT_NONE = make_units(0, UNIT_CODE_UNITLESS);
constexpr ControlUnits UNIT_DB = make_units(0, UNIT_CODE_DB);
constexpr ControlUnits UNIT_HZ = make_units(0, UNIT_CODE_HERTZ);
constexpr ControlUnits UNIT_OCTAVES = make_units(0, UNIT_CODE_OCTAVES);
constexpr ControlUnits UNIT_MS = make_units(-3, UNIT_CODE_SECONDS);
constexpr ControlUnits UNIT_KELVIN = make_units(0, UNIT_CODE_KELVIN);
constexpr ControlUnits UNIT_METRES = make_units(0, UNIT_CODE_METRES);
constexpr ControlUnits UNIT_PERCENT = make_units(0, UNIT_CODE_PERCENT);

// ---- generator ----------------------------------------------------------

/// One value slot of a linear-float CONTROL: format + the GALAXY path it
/// binds to (empty path = unbound, e.g. IDENTIFY).
struct FloatValue
{
    float min, max, step, def;
    ControlUnits unit;
    std::string path;
};

class Generator
{
  public:
    Generator(GalaxyNamespace const& ns, GalaxyModelBanks const& banks)
        : ns_(ns)
        , banks_(banks)
        , binder_(ns.size())
    {}

    GalaxyAemModel run();

  private:
    // -- binding helpers --
    void bind_value(
        std::uint16_t descriptor_type, std::uint16_t descriptor_index, std::uint16_t value_index, std::string const& path)
    {
        if (path.empty()) {
            return;
        }
        auto h = ns_.adaptor.resolve(path);
        assert(h.has_value());
        bool ok = binder_.bind(AemRef{0, descriptor_type, descriptor_index, value_index}, *h);
        assert(ok);
        (void)ok;
    }

    /// The live-device range for a bound path (folded describe metadata in
    /// the schema model); nullopt when the point is unbound or unranged.
    struct LiveRange
    {
        float min, max, step;
        ControlUnits unit;
    };
    std::optional<LiveRange> live_range(std::string const& path) const
    {
        if (path.empty()) {
            return std::nullopt;
        }
        auto h = ns_.adaptor.resolve(path);
        if (!h) {
            return std::nullopt;
        }
        auto const& m = *ns_.model;
        if (!(m.min(*h) < m.max(*h))) {
            return std::nullopt;
        }
        return LiveRange{
            float(m.min(*h)), float(m.max(*h)), float(m.step(*h)), make_units(m.multiplier(*h), std::uint8_t(m.units(*h)))};
    }

    // -- localized strings (Clause 7.3.7) --
    // Every named descriptor's manufacturer name also lands in the string
    // table: object_name is the user's relabelable sticker (SET_NAME),
    // localized_description is the fixed identity underneath it. The
    // table chunks into STRINGS descriptors of 7 at emission; the packed
    // reference is (strings_descriptor_index << 3) | string_offset.
    std::uint16_t loc(std::string_view name)
    {
        auto const n = strings_.size();
        assert(n / 7 < 0x1FFF);  // 13-bit STRINGS index
        strings_.emplace_back(name);
        return std::uint16_t(((n / 7) << 3) | (n % 7));
    }

    // -- control emission --
    DescriptorControl control_header(
        std::string_view name, Eui64 const& type, std::uint16_t value_type, bool read_only, std::uint16_t n)
    {
        DescriptorControl c;
        c.descriptor_index = next_control_;
        c.object_name = AtdeccString{name};
        c.localized_description = loc(name);
        c.control_value_type = pack_control_value_type(read_only, false, value_type);
        c.control_type = type;
        c.number_of_values = n;
        c.signal_source = NO_SIGNAL;
        return c;
    }

    void emit_control(DescriptorControl const& c)
    {
        auto view = make_const_span(c);
        controls_.push_back({std::vector<std::uint8_t>(view.begin(), view.begin() + long(c.wire_size()))});
        ++next_control_;
    }

    /// Grouped linear-float control: one value block per FloatValue.
    std::uint16_t add_float_control(
        std::string_view name, Eui64 const& type, std::initializer_list<FloatValue> values, bool read_only = false)
    {
        auto c = control_header(name, type, CONTROL_LINEAR_FLOAT, read_only, std::uint16_t(values.size()));
        auto trailer = c.value_details_bytes_mutable();
        std::size_t at = 0;
        std::uint16_t v = 0;
        for (auto const& fv : values) {
            // The live device's describe metadata wins over the authored
            // placeholder range; the guide-corroborated default stays.
            auto const live = live_range(fv.path);
            LinearValueEntry<float> entry;
            entry.set_minimum(live ? live->min : fv.min);
            entry.set_maximum(live ? live->max : fv.max);
            entry.set_step(live ? live->step : fv.step);
            entry.set_default_value(fv.def);
            entry.set_current(fv.def);
            entry.unit = live ? live->unit : fv.unit;
            entry.localized_string = NO_LOCALIZED;
            std::memcpy(trailer.data() + at, &entry, sizeof entry);
            at += sizeof entry;
            bind_value(DESCRIPTOR_CONTROL, next_control_, v++, fv.path);
        }
        auto idx = next_control_;
        emit_control(c);
        return idx;
    }

    /// Single-value uint8 control (bools are 0/255 per AEM convention).
    std::uint16_t add_u8_control(
        std::string_view name,
        Eui64 const& type,
        std::uint8_t min,
        std::uint8_t max,
        std::uint8_t step,
        std::uint8_t def,
        std::string const& path,
        bool read_only = false)
    {
        auto c = control_header(name, type, CONTROL_LINEAR_UINT8, read_only, 1);
        if (auto const live = live_range(path); live && live->min >= 0 && live->max <= 255) {
            min = std::uint8_t(live->min);
            max = std::uint8_t(live->max);
            step = std::uint8_t(live->step > 0 ? live->step : 1);
        }
        LinearValueEntry<std::uint8_t> entry;
        entry.set_minimum(min);
        entry.set_maximum(max);
        entry.set_step(step);
        entry.set_default_value(def);
        entry.set_current(def);
        entry.unit = UNIT_NONE;
        entry.localized_string = NO_LOCALIZED;
        std::memcpy(c.value_details_bytes_mutable().data(), &entry, sizeof entry);
        bind_value(DESCRIPTOR_CONTROL, next_control_, 0, path);
        auto idx = next_control_;
        emit_control(c);
        return idx;
    }

    std::uint16_t add_bool_control(
        std::string_view name, Eui64 const& type, bool def, std::string const& path, bool read_only = false)
    {
        return add_u8_control(name, type, 0, 255, 255, def ? 255 : 0, path, read_only);
    }

    /// Array-of-float control (meters): one shared format block + N
    /// current values; per-index paths bind value_index 0..N-1.
    std::uint16_t add_float_array_control(
        std::string_view name,
        Eui64 const& type,
        float min,
        float max,
        float step,
        float def,
        ControlUnits unit,
        std::vector<std::string> const& paths,
        bool read_only = true)
    {
        auto n = std::uint16_t(paths.size());
        auto c = control_header(name, type, CONTROL_ARRAY_FLOAT, read_only, n);
        // Array layout (Table 7.12/7.13): min, max, step, default (V each),
        // unit (2), string (2), then N current values.
        auto trailer = c.value_details_bytes_mutable();
        std::size_t at = 0;
        auto put_float = [&](float value) {
            std::array<std::uint8_t, 4> bytes{};
            detail::encode_linear_field(value, bytes);
            std::memcpy(trailer.data() + at, bytes.data(), 4);
            at += 4;
        };
        put_float(min);
        put_float(max);
        put_float(step);
        put_float(def);
        trailer[at++] = unit.multiplier;
        trailer[at++] = unit.code;
        trailer[at++] = 0xFF;  // localized_string = NONE
        trailer[at++] = 0xFF;
        for (std::uint16_t v = 0; v < n; ++v) {
            put_float(def);
            bind_value(DESCRIPTOR_CONTROL, next_control_, v, paths[v]);
        }
        auto idx = next_control_;
        emit_control(c);
        return idx;
    }

    std::uint16_t add_utf8_control(std::string_view name, Eui64 const& type, std::string const& path, bool read_only = true)
    {
        auto c = control_header(name, type, CONTROL_UTF8, read_only, 1);
        bind_value(DESCRIPTOR_CONTROL, next_control_, 0, path);
        auto idx = next_control_;
        emit_control(c);
        return idx;
    }

    // -- sections --
    void add_input_strip(unsigned i);
    void add_output_strip(unsigned o);
    void add_link_groups();
    void add_globals();
    void add_matrices();
    void add_signal_selectors();

    GalaxyNamespace const& ns_;
    GalaxyModelBanks const& banks_;
    AemBindingBuilder binder_;

    struct RawControl
    {
        std::vector<std::uint8_t> bytes;
    };
    std::vector<RawControl> controls_;
    std::uint16_t next_control_ = 0;

    struct Block
    {
        std::string name;
        std::uint16_t base;
        std::uint16_t count;
    };
    std::vector<Block> blocks_;

    std::vector<DescriptorMatrix> matrices_;
    std::vector<std::vector<std::uint8_t>> matrix_trailers_;
    std::vector<DescriptorSignalSelector> selectors_;
    std::vector<std::string> strings_;  ///< localized-string table, 7 per STRINGS descriptor
};

std::string path_of(std::string_view fmt_prefix, unsigned n, std::string_view tail)
{
    return std::string(fmt_prefix) + std::to_string(n) + std::string(tail);
}

void Generator::add_input_strip(unsigned i)
{
    auto base = next_control_;
    auto p = [&](std::string_view tail) { return path_of("/processing/input/", i, tail); };
    auto d = [&](std::string_view tail) { return path_of("/device/input/", i, tail); };
    auto label = [&](std::string_view what) { return "Input " + std::to_string(i) + " " + std::string(what); };

    add_float_control(label("Gain"), CONTROL_TYPE_GAIN, {{-90.f, 10.f, 0.1f, 0.f, UNIT_DB, p("/gain")}});
    add_bool_control(label("Mute"), CONTROL_TYPE_MUTE, true, p("/mute"));
    add_float_control(label("Delay"), CONTROL_TYPE_DELAY, {{0.f, 2000.f, 0.01f, 0.f, UNIT_MS, p("/delay")}});
    add_u8_control(label("Delay Type"), VT_DELAY_DISPLAY_TYPE, 0, 2, 1, 0, p("/delay_type"));
    static float const IN_EQ_FREQ[5] = {32.f, 125.f, 500.f, 2000.f, 8000.f};
    for (unsigned b = 1; b <= 5; ++b) {
        auto eq = [&](std::string_view tail) { return p("/eq/" + std::to_string(b) + std::string(tail)); };
        add_float_control(
            label("EQ Band " + std::to_string(b)),
            VT_EQ_BAND,
            {{10.f, 20000.f, 1.f, IN_EQ_FREQ[b - 1], UNIT_HZ, eq("/frequency")},
             {0.1f, 4.f, 0.01f, 1.f, UNIT_OCTAVES, eq("/bandwidth")},
             {-18.f, 18.f, 0.1f, 0.f, UNIT_DB, eq("/gain")}});
        add_bool_control(label("EQ Band " + std::to_string(b) + " Bypass"), CONTROL_TYPE_ENABLE, false, eq("/band_bypass"));
    }
    add_bool_control(label("EQ Bypass"), CONTROL_TYPE_ENABLE, false, p("/eq/bypass"));
    add_bool_control(label("Equalization Bypass"), CONTROL_TYPE_ENABLE, false, p("/equalization_bypass"));
    static float const USHAPE_FREQ[4] = {62.f, 250.f, 1000.f, 4000.f};
    for (unsigned u = 1; u <= 4; ++u) {
        auto us = [&](std::string_view tail) { return p("/ushaping/" + std::to_string(u) + std::string(tail)); };
        add_float_control(
            label("U-Shaping Band " + std::to_string(u)),
            VT_USHAPING_BAND,
            {{10.f, 20000.f, 1.f, USHAPE_FREQ[u - 1], UNIT_HZ, us("/frequency")},
             {-18.f, 18.f, 0.1f, 0.f, UNIT_DB, us("/gain")},
             {1.f, 4.f, 1.f, 2.f, UNIT_NONE, us("/slope")}});
    }
    add_float_control(
        label("U-Shaping Band 5 Gain"), VT_USHAPING_BAND5_GAIN, {{-18.f, 18.f, 0.1f, 0.f, UNIT_DB, p("/ushaping/5/gain")}});
    add_bool_control(label("U-Shaping Bypass"), CONTROL_TYPE_ENABLE, false, p("/ushaping/bypass"));
    add_u8_control(label("Link Group"), VT_LINK_GROUP_ASSIGN, 0, 4, 1, 0, d("/input_link_group"));
    add_bool_control(label("Isolate"), VT_ISOLATE, false, d("/isolate"));
    add_bool_control(label("Select"), VT_SELECT, false, d("/select"));
    add_bool_control(label("AES ASRC"), CONTROL_TYPE_SRC_MODE, true, d("/aes/enable_asrc"));
    add_float_control(label("Scale"), CONTROL_TYPE_AUDIO_SCALE, {{0.f, 26.f, 1.f, 26.f, UNIT_DB, d("/scale")}});

    blocks_.push_back({"Input " + std::to_string(i), base, std::uint16_t(next_control_ - base)});
}

void Generator::add_output_strip(unsigned o)
{
    auto base = next_control_;
    auto p = [&](std::string_view tail) { return path_of("/processing/output/", o, tail); };
    auto d = [&](std::string_view tail) { return path_of("/device/output/", o, tail); };
    auto label = [&](std::string_view what) { return "Output " + std::to_string(o) + " " + std::string(what); };

    add_float_control(label("Gain"), CONTROL_TYPE_GAIN, {{-90.f, 10.f, 0.1f, 0.f, UNIT_DB, p("/gain")}});
    add_bool_control(label("Mute"), CONTROL_TYPE_MUTE, false, p("/mute"));
    add_bool_control(label("Polarity"), CONTROL_TYPE_INVERT, false, p("/polarity_reversal"));
    add_float_control(label("Delay"), CONTROL_TYPE_DELAY, {{0.f, 2000.f, 0.01f, 0.f, UNIT_MS, p("/delay")}});
    add_u8_control(label("Delay Type"), VT_DELAY_DISPLAY_TYPE, 0, 2, 1, 0, p("/delay_type"));
    static float const OUT_EQ_FREQ[10] = {32.f, 63.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f};
    for (unsigned b = 1; b <= 10; ++b) {
        auto eq = [&](std::string_view tail) { return p("/eq/" + std::to_string(b) + std::string(tail)); };
        add_float_control(
            label("EQ Band " + std::to_string(b)),
            VT_EQ_BAND,
            {{10.f, 20000.f, 1.f, OUT_EQ_FREQ[b - 1], UNIT_HZ, eq("/frequency")},
             {0.1f, 4.f, 0.01f, 1.f, UNIT_OCTAVES, eq("/bandwidth")},
             {-18.f, 18.f, 0.1f, 0.f, UNIT_DB, eq("/gain")}});
        add_bool_control(label("EQ Band " + std::to_string(b) + " Bypass"), CONTROL_TYPE_ENABLE, false, eq("/band_bypass"));
    }
    add_bool_control(label("EQ Bypass"), CONTROL_TYPE_ENABLE, false, p("/eq/bypass"));
    add_bool_control(label("Equalization Bypass"), CONTROL_TYPE_ENABLE, false, p("/equalization_bypass"));
    add_bool_control(label("Highpass Bypass"), CONTROL_TYPE_ENABLE, true, p("/highpass/bypass"));
    add_float_control(
        label("Highpass Frequency"), CONTROL_TYPE_FREQUENCY, {{10.f, 20000.f, 1.f, 40.f, UNIT_HZ, p("/highpass/frequency")}});
    add_u8_control(label("Highpass Type"), VT_FILTER_TYPE, 0, 20, 1, 11, p("/highpass/type"));
    add_bool_control(label("Lowpass Bypass"), CONTROL_TYPE_ENABLE, true, p("/lowpass/bypass"));
    add_float_control(
        label("Lowpass Frequency"), CONTROL_TYPE_FREQUENCY, {{10.f, 20000.f, 1.f, 160.f, UNIT_HZ, p("/lowpass/frequency")}});
    add_u8_control(label("Lowpass Type"), VT_FILTER_TYPE, 0, 20, 1, 11, p("/lowpass/type"));
    add_bool_control(label("Atmospheric Bypass"), CONTROL_TYPE_ENABLE, true, p("/atmospheric/bypass"));
    add_float_control(
        label("Atmospheric Distance"),
        VT_ATMOSPHERIC_DISTANCE,
        {{0.f, 1000.f, 0.1f, 0.f, UNIT_METRES, p("/atmospheric/distance")}});
    add_float_control(label("Atmospheric Gain"), CONTROL_TYPE_GAIN, {{0.f, 10.f, 0.1f, 10.f, UNIT_DB, p("/atmospheric/gain")}});
    static float const USHAPE_FREQ[4] = {62.f, 250.f, 1000.f, 4000.f};
    for (unsigned u = 1; u <= 4; ++u) {
        auto us = [&](std::string_view tail) { return p("/ushaping/" + std::to_string(u) + std::string(tail)); };
        add_float_control(
            label("U-Shaping Band " + std::to_string(u)),
            VT_USHAPING_BAND,
            {{10.f, 20000.f, 1.f, USHAPE_FREQ[u - 1], UNIT_HZ, us("/frequency")},
             {-18.f, 18.f, 0.1f, 0.f, UNIT_DB, us("/gain")},
             {1.f, 4.f, 1.f, 2.f, UNIT_NONE, us("/slope")}});
    }
    add_float_control(
        label("U-Shaping Band 5 Gain"), VT_USHAPING_BAND5_GAIN, {{-18.f, 18.f, 0.1f, 0.f, UNIT_DB, p("/ushaping/5/gain")}});
    add_bool_control(label("U-Shaping Bypass"), CONTROL_TYPE_ENABLE, false, p("/ushaping/bypass"));
    add_u8_control(label("Link Group"), VT_LINK_GROUP_ASSIGN, 0, 8, 1, 0, d("/output_link_group"));
    add_bool_control(label("Isolate"), VT_ISOLATE, false, d("/isolate"));
    add_bool_control(label("Mute Relay"), VT_MUTE_RELAY, false, d("/mute_relay"));
    add_bool_control(label("Select"), VT_SELECT, false, d("/select"));
    add_float_control(label("Scale"), CONTROL_TYPE_AUDIO_SCALE, {{0.f, 26.f, 1.f, 26.f, UNIT_DB, d("/scale")}});
    add_float_control(label("SIM Trim"), VT_SIM_TRIM, {{-12.f, 12.f, 0.1f, 0.f, UNIT_DB, d("/sim/trim")}});

    blocks_.push_back({"Output " + std::to_string(o), base, std::uint16_t(next_control_ - base)});
}

void Generator::add_link_groups()
{
    for (unsigned g = 1; g <= 4; ++g) {
        auto base = next_control_;
        add_bool_control(
            "Input Link Group " + std::to_string(g) + " Bypass",
            CONTROL_TYPE_ENABLE,
            true,
            path_of("/device/input_link_group/", g, "/bypass"));
        blocks_.push_back({"Input Link Group " + std::to_string(g), base, 1});
    }
    for (unsigned g = 1; g <= 8; ++g) {
        auto base = next_control_;
        add_bool_control(
            "Output Link Group " + std::to_string(g) + " Bypass",
            CONTROL_TYPE_ENABLE,
            true,
            path_of("/device/output_link_group/", g, "/bypass"));
        blocks_.push_back({"Output Link Group " + std::to_string(g), base, 1});
    }
}

void Generator::add_globals()
{
    // The atmospheric block is the cleanest standard fit in the model:
    // ALTITUDE / RELATIVE_HUMIDITY / TEMPERATURE, with GALAXY already in
    // Kelvin (PLAN §5.3).
    add_float_control(
        "Atmospheric Altitude",
        CONTROL_TYPE_ALTITUDE,
        {{-500.f, 5000.f, 1.f, 0.f, UNIT_METRES, "/device/output/atmospheric/altitude"}});
    add_float_control(
        "Atmospheric Humidity",
        CONTROL_TYPE_RELATIVE_HUMIDITY,
        {{0.f, 100.f, 1.f, 50.f, UNIT_PERCENT, "/device/output/atmospheric/humidity"}});
    add_float_control(
        "Atmospheric Temperature",
        CONTROL_TYPE_TEMPERATURE,
        {{223.15f, 323.15f, 0.1f, 293.15f, UNIT_KELVIN, "/device/output/atmospheric/temperature"}});
    add_u8_control("Display Brightness", CONTROL_TYPE_BRIGHTNESS, 0, 3, 1, 1, "/device/preferences/brightness");
    add_u8_control("Display Color", VT_DISPLAY_COLOR, 0, 7, 1, 3, "/device/preferences/display_color");
    add_bool_control("Front Panel Lockout", VT_FRONT_PANEL_LOCKOUT, false, "/system/hardware/front_panel_lockout");
    // Snapshot recall (PLAN §5.4): recall by writing an id; the exclusion
    // bitmask and management stay on the vendor channel.
    {
        auto c = control_header("Snapshot Recall", CONTROL_TYPE_SNAPSHOT, CONTROL_LINEAR_INT16, false, 1);
        LinearValueEntry<std::int16_t> entry;
        entry.set_minimum(-1);
        entry.set_maximum(255);
        entry.set_step(1);
        entry.set_default_value(-1);
        entry.set_current(-1);
        entry.unit = UNIT_NONE;
        entry.localized_string = NO_LOCALIZED;
        std::memcpy(c.value_details_bytes_mutable().data(), &entry, sizeof entry);
        bind_value(DESCRIPTOR_CONTROL, next_control_, 0, "/project/snapshot/active/id");
        emit_control(c);
    }

    // Meters are arrays, one control per bank (PLAN §5.3).
    auto meter_paths = [&](std::string_view prefix, unsigned n, std::string_view tail = "") {
        std::vector<std::string> paths;
        for (unsigned i = 1; i <= n; ++i) {
            paths.push_back(std::string(prefix) + std::to_string(i) + std::string(tail));
        }
        return paths;
    };
    add_float_array_control(
        "Input Meters",
        CONTROL_TYPE_AUDIO_METERS,
        -90.f,
        20.f,
        0.5f,
        -90.f,
        UNIT_DB,
        meter_paths("/status/meter/input/", banks_.input_meters));
    add_float_array_control(
        "Input Split Meters",
        CONTROL_TYPE_AUDIO_METERS,
        -90.f,
        20.f,
        0.5f,
        0.f,
        UNIT_DB,
        meter_paths("/status/meter/input_split/", banks_.input_split_meters));
    add_float_array_control(
        "Matrix Input Meters",
        CONTROL_TYPE_AUDIO_METERS,
        -90.f,
        20.f,
        0.5f,
        -90.f,
        UNIT_DB,
        meter_paths("/status/meter/matrix_input/", banks_.matrix_input_meters));
    add_float_array_control(
        "Output Meters",
        CONTROL_TYPE_AUDIO_METERS,
        -90.f,
        20.f,
        0.5f,
        -90.f,
        UNIT_DB,
        meter_paths("/status/meter/output/", banks_.output_meters));
    add_float_array_control(
        "SIM Meters",
        CONTROL_TYPE_AUDIO_METERS,
        -90.f,
        20.f,
        0.5f,
        -90.f,
        UNIT_DB,
        meter_paths("/status/meter/sim/", banks_.sim_meters));
    add_float_array_control(
        "Line Monitor Ideal",
        CONTROL_TYPE_AUDIO_METERS,
        -90.f,
        20.f,
        0.5f,
        0.f,
        UNIT_DB,
        meter_paths("/status/meter/line_mon/", banks_.line_mon, "/ideal"));
    add_float_array_control(
        "Line Monitor Neg",
        CONTROL_TYPE_AUDIO_METERS,
        -90.f,
        20.f,
        0.5f,
        0.f,
        UNIT_DB,
        meter_paths("/status/meter/line_mon/", banks_.line_mon, "/neg"));
    add_float_array_control(
        "Line Monitor Pos",
        CONTROL_TYPE_AUDIO_METERS,
        -90.f,
        20.f,
        0.5f,
        0.f,
        UNIT_DB,
        meter_paths("/status/meter/line_mon/", banks_.line_mon, "/pos"));

    // Resource budget exposure: read-only crosspoints-used counter; the
    // 232-crosspoint ceiling itself is enforced at command time (PLAN §5.3).
    add_float_control(
        "Matrix Crosspoints Used",
        VT_CROSSPOINTS_USED,
        {{0.f, 232.f, 1.f, 16.f, UNIT_NONE, "/status/matrix_crosspoints_used"}},
        true);

    add_float_control(
        "ARM Temperature",
        CONTROL_TYPE_TEMPERATURE,
        {{233.15f, 373.15f, 0.1f, 299.15f, UNIT_KELVIN, "/status/hardware/board/digital/arm_temp"}},
        true);
    add_float_control(
        "FPGA Temperature",
        CONTROL_TYPE_TEMPERATURE,
        {{233.15f, 373.15f, 0.1f, 313.5f, UNIT_KELVIN, "/status/hardware/board/digital/fpga_temp"}},
        true);
    add_float_control(
        "Primary IO Temperature",
        CONTROL_TYPE_TEMPERATURE,
        {{233.15f, 373.15f, 0.1f, 309.15f, UNIT_KELVIN, "/status/hardware/board/primary_io/analog_out_temp"}},
        true);
    add_float_control(
        "Secondary IO Temperature",
        CONTROL_TYPE_TEMPERATURE,
        {{233.15f, 373.15f, 0.1f, 309.15f, UNIT_KELVIN, "/status/hardware/board/secondary_io/analog_out_temp"}},
        true);
    add_float_array_control(
        "Fan Tach",
        CONTROL_TYPE_FAN_STATUS,
        0.f,
        20000.f,
        1.f,
        3750.f,
        UNIT_NONE,
        meter_paths("/status/hardware/board/digital/fan/", 4, "/tach"));
    add_float_array_control(
        "Board Voltages",
        VT_BOARD_VOLTAGE,
        0.f,
        6.f,
        0.001f,
        1.f,
        UNIT_NONE,
        {"/status/hardware/board/digital/fpga_aux_voltage",
         "/status/hardware/board/digital/fpga_main_voltage",
         "/status/hardware/board/digital/fpga_ram_voltage",
         "/status/hardware/board/digital/fpga_ref_voltage",
         "/status/hardware/board/digital/main_5v0_voltage"});
    add_float_array_control(
        "LED State",
        VT_LED_STATE,
        0.f,
        7.f,
        1.f,
        0.f,
        UNIT_NONE,
        {"/status/led/avb_sync", "/status/led/media_clock", "/status/led/network", "/status/led/sim_bus"});
    add_utf8_control("Log Message", VT_LOG_MESSAGE, "/status/log_message");
}

void Generator::add_matrices()
{
    // Four MATRIX descriptors, one per crosspoint parameter (a MATRIX
    // carries one control_type and one value format across the whole
    // grid); all reference the same 48 MATRIX_SIGNAL descriptors,
    // width 16, height 32 (PLAN §5.3). Crosspoint (in, out) binds as
    // value_index (in-1)*16 + (out-1). The default asymmetry (matrix in 1
    // cp 1-8 / in 2 cp 9-16 at 0 dB) cannot be expressed — AEM declares
    // one default for the grid.
    struct MatrixSpec
    {
        char const* name;
        char const* param;
        Eui64 control_type;
        std::uint16_t value_type;
        float min, max, step, def;
        ControlUnits unit;
    };
    MatrixSpec const SPECS[] = {
        {"Matrix Gain", "gain", CONTROL_TYPE_GAIN, CONTROL_LINEAR_FLOAT, -90.f, 10.f, 0.1f, -90.f, UNIT_DB},
        {"Matrix Delay", "delay", CONTROL_TYPE_DELAY, CONTROL_LINEAR_FLOAT, 0.f, 500.f, 0.01f, 0.f, UNIT_MS},
        {"Matrix Delay Bypass", "delay_bypass", CONTROL_TYPE_ENABLE, CONTROL_LINEAR_UINT8, 0.f, 255.f, 255.f, 0.f, UNIT_NONE},
        {"Matrix Delay Type", "delay_type", VT_MATRIX_DELAY_TYPE, CONTROL_LINEAR_UINT8, 0.f, 2.f, 1.f, 0.f, UNIT_NONE},
    };

    std::uint16_t m = 0;
    for (auto spec : SPECS) {
        // Live describe ranges for the crosspoint parameter, when folded.
        if (auto const live = live_range("/processing/matrix/1/1/" + std::string(spec.param))) {
            spec.min = live->min;
            spec.max = live->max;
            spec.step = live->step;
            spec.unit = live->unit;
        }
        DescriptorMatrix desc;
        desc.descriptor_index = m;
        desc.object_name = AtdeccString{spec.name};
        desc.localized_description = loc(spec.name);
        desc.control_value_type = pack_control_value_type(false, false, spec.value_type);
        desc.control_type = spec.control_type;
        desc.width = 16;
        desc.height = 32;
        desc.values_offset = std::uint16_t(DescriptorMatrix::LENGTH);
        desc.number_of_values = 1;
        desc.number_of_sources = 48;
        desc.base_source = 0;

        std::vector<std::uint8_t> trailer;
        if (spec.value_type == CONTROL_LINEAR_FLOAT) {
            LinearValueEntry<float> entry;
            entry.set_minimum(spec.min);
            entry.set_maximum(spec.max);
            entry.set_step(spec.step);
            entry.set_default_value(spec.def);
            entry.set_current(spec.def);
            entry.unit = spec.unit;
            entry.localized_string = NO_LOCALIZED;
            trailer.resize(sizeof entry);
            std::memcpy(trailer.data(), &entry, sizeof entry);
        } else {
            LinearValueEntry<std::uint8_t> entry;
            entry.set_minimum(std::uint8_t(spec.min));
            entry.set_maximum(std::uint8_t(spec.max));
            entry.set_step(std::uint8_t(spec.step));
            entry.set_default_value(std::uint8_t(spec.def));
            entry.set_current(std::uint8_t(spec.def));
            entry.unit = spec.unit;
            entry.localized_string = NO_LOCALIZED;
            trailer.resize(sizeof entry);
            std::memcpy(trailer.data(), &entry, sizeof entry);
        }

        for (unsigned in = 1; in <= 32; ++in) {
            for (unsigned out = 1; out <= 16; ++out) {
                auto path = "/processing/matrix/" + std::to_string(in) + "/" + std::to_string(out) + "/" + spec.param;
                bind_value(DESCRIPTOR_MATRIX, m, std::uint16_t((in - 1) * 16 + (out - 1)), path);
            }
        }
        matrices_.push_back(desc);
        matrix_trailers_.push_back(std::move(trailer));
        ++m;
    }
}

void Generator::add_signal_selectors()
{
    // /device/input/N/mode for the 8 processed inputs (PLAN §5.2). The
    // proxy models no signal path, so the mode options are authored as
    // sources with signal_type INVALID and signal_index = the mode value
    // (AEM's escape hatch for "no modeled descriptor"): mode 0 — NULL —
    // is literally NO_SIGNAL, and every option is self-describing, so
    // the mirror maps source <-> mode with no side table. The schema's
    // INPUT_MODE_* enum labels attach through the widget label provider.
    for (unsigned i = 1; i <= 8; ++i) {
        DescriptorSignalSelector s;
        auto const name = "Input " + std::to_string(i) + " Mode";
        s.descriptor_index = std::uint16_t(i - 1);
        s.object_name = AtdeccString{name};
        s.localized_description = loc(name);
        s.sources_offset = std::uint16_t(DescriptorSignalSelector::LENGTH);
        s.number_of_sources = GALAXY_INPUT_MODE_COUNT;
        s.current_signal = NO_SIGNAL;  // mode 0 (NULL) until the mirror reports
        s.default_signal = NO_SIGNAL;
        bind_value(DESCRIPTOR_SIGNAL_SELECTOR, std::uint16_t(i - 1), 0, path_of("/device/input/", i, "/mode"));
        selectors_.push_back(s);
    }
}

GalaxyAemModel Generator::run()
{
    // Control order defines CONTROL_BLOCK contiguity: input strips, output
    // strips, link groups, then the global block-less controls.
    add_u8_control("Identify", CONTROL_TYPE_IDENTIFY, 0, 255, 255, 0, "");  // index 0, customary
    for (unsigned i = 1; i <= 8; ++i) {
        add_input_strip(i);
    }
    for (unsigned o = 1; o <= 16; ++o) {
        add_output_strip(o);
    }
    add_link_groups();
    add_globals();
    add_matrices();
    add_signal_selectors();

    GalaxyAemModel model;
    auto add_raw = [&](std::uint16_t type, std::uint16_t index, std::span<std::uint8_t const> bytes) {
        model.descriptors.push_back({0, type, index, {bytes.begin(), bytes.end()}});
    };

    // Top-level names enter the string table before CONFIGURATION is
    // built — its descriptor_counts trailer pins the final STRINGS count.
    auto const vendor_loc = loc("Statusbar");
    auto const model_loc = loc(std::string("GALAXY Proxy ") + banks_.model_string);
    auto const config_loc = loc("GALAXY 816 Proxy");
    auto const unit_loc = loc("GALAXY DSP");
    auto const avb_if_loc = loc("Ethernet");
    std::vector<std::uint16_t> block_locs;
    block_locs.reserve(blocks_.size());
    for (auto const& b : blocks_) {
        block_locs.push_back(loc(b.name));
    }
    auto const strings_count = std::uint16_t((strings_.size() + 6) / 7);

    // ---- ENTITY ----
    DescriptorEntity entity;
    entity.entity_id = Eui64{};  // patched at runtime from the GALAXY's id (PLAN §7.1)
    // Per-variant id (GalaxyModelBanks): distinct meter-bank widths are
    // distinct served models, and controllers cache by entity_model_id.
    entity.entity_model_id = Eui64{
        std::uint8_t(banks_.entity_model_id >> 56),
        std::uint8_t(banks_.entity_model_id >> 48),
        std::uint8_t(banks_.entity_model_id >> 40),
        std::uint8_t(banks_.entity_model_id >> 32),
        std::uint8_t(banks_.entity_model_id >> 24),
        std::uint8_t(banks_.entity_model_id >> 16),
        std::uint8_t(banks_.entity_model_id >> 8),
        std::uint8_t(banks_.entity_model_id)};
    entity.entity_capabilities = atdecc::entity_capabilities::AEM_SUPPORTED;
    entity.entity_name = AtdeccString{"GALAXY Proxy"};
    entity.vendor_name_string = vendor_loc;
    entity.model_name_string = model_loc;
    entity.firmware_version = AtdeccString{STATUSBAR_ATDECC_CONTROL_VERSION};
    entity.group_name = AtdeccString{""};
    entity.serial_number = AtdeccString{""};
    entity.configurations_count = 1;
    entity.current_configuration = 0;
    add_raw(DESCRIPTOR_ENTITY, 0, make_const_span(entity));

    // ---- CONFIGURATION (with the full descriptor_counts trailer) ----
    DescriptorConfiguration config;
    config.object_name = AtdeccString{"GALAXY 816 Proxy"};
    config.localized_description = config_loc;
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_AUDIO_UNIT), 1});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_AVB_INTERFACE), 1});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_CONTROL), std::uint16_t(controls_.size())});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_SIGNAL_SELECTOR), std::uint16_t(selectors_.size())});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_MATRIX), std::uint16_t(matrices_.size())});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_MATRIX_SIGNAL), 48});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_CONTROL_BLOCK), std::uint16_t(blocks_.size())});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_LOCALE), 1});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_STRINGS), strings_count});
    add_raw(DESCRIPTOR_CONFIGURATION, 0, std::span(make_const_span(config)).first(config.wire_size()));

    // ---- AUDIO_UNIT ----
    DescriptorAudioUnit unit;
    unit.object_name = AtdeccString{"GALAXY DSP"};
    unit.localized_description = unit_loc;
    unit.number_of_controls = std::uint16_t(controls_.size());
    unit.base_control = 0;
    unit.number_of_signal_selectors = std::uint16_t(selectors_.size());
    unit.base_signal_selector = 0;
    unit.number_of_matrices = std::uint16_t(matrices_.size());
    unit.base_matrix = 0;
    unit.number_of_control_blocks = std::uint16_t(blocks_.size());
    unit.base_control_block = 0;
    unit.current_sampling_rate = 0x00017700;  // 96 kHz, pull 0
    (void)unit.push_sampling_rate(ieee::quadlet_t{0x00017700});
    add_raw(DESCRIPTOR_AUDIO_UNIT, 0, std::span(make_const_span(unit)).first(unit.wire_size()));

    // ---- AVB_INTERFACE (the proxy host's NIC) ----
    // The generated model carries the JDKS experimental MAC
    // (70:B3:D5:ED:CF:F0-F7 is reserved for experimental devices); the
    // runtime patches in the real NIC MAC and gPTP clock identity.
    DescriptorAvbInterface avb_if;
    avb_if.object_name = AtdeccString{"Ethernet"};
    avb_if.localized_description = avb_if_loc;
    avb_if.mac_address = ieee::Eui48{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0xF0};
    avb_if.clock_identity = Eui64{0x70, 0xB3, 0xD5, 0xFF, 0xFE, 0xED, 0xCF, 0xF0};
    add_raw(DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(avb_if));

    // ---- CONTROLs ----
    for (std::uint16_t i = 0; i < controls_.size(); ++i) {
        add_raw(DESCRIPTOR_CONTROL, i, controls_[i].bytes);
    }

    // ---- CONTROL_BLOCKs ----
    for (std::uint16_t i = 0; i < blocks_.size(); ++i) {
        DescriptorControlBlock block;
        block.descriptor_index = i;
        block.object_name = AtdeccString{blocks_[i].name};
        block.localized_description = block_locs[i];
        block.number_of_controls = blocks_[i].count;
        block.base_control = blocks_[i].base;
        block.final_control_index = std::uint16_t(blocks_[i].base + blocks_[i].count - 1);
        block.signal_source = NO_SIGNAL;
        add_raw(DESCRIPTOR_CONTROL_BLOCK, i, make_const_span(block));
    }

    // ---- MATRICEs + trailers ----
    for (std::uint16_t i = 0; i < matrices_.size(); ++i) {
        auto head = make_const_span(matrices_[i]);
        std::vector<std::uint8_t> bytes(head.begin(), head.begin() + long(DescriptorMatrix::LENGTH));
        bytes.insert(bytes.end(), matrix_trailers_[i].begin(), matrix_trailers_[i].end());
        add_raw(DESCRIPTOR_MATRIX, i, bytes);
    }

    // ---- MATRIX_SIGNALs (48: 32 sources + 16 destinations; the proxy
    // has no audio path, so each lists one INVALID signal) ----
    for (std::uint16_t i = 0; i < 48; ++i) {
        DescriptorMatrixSignal sig;
        sig.descriptor_index = i;
        sig.signals_offset = std::uint16_t(DescriptorMatrixSignal::LENGTH);
        sig.signals_count = 1;
        std::vector<std::uint8_t> bytes;
        auto head = make_const_span(sig);
        bytes.assign(head.begin(), head.end());
        auto sigsrc = make_const_span(NO_SIGNAL);
        bytes.insert(bytes.end(), sigsrc.begin(), sigsrc.end());
        add_raw(DESCRIPTOR_MATRIX_SIGNAL, i, bytes);
    }

    // ---- SIGNAL_SELECTORs (head + the authored mode-source trailer) ----
    for (std::uint16_t i = 0; i < selectors_.size(); ++i) {
        std::vector<std::uint8_t> bytes;
        auto head = make_const_span(selectors_[i]);
        bytes.assign(head.begin(), head.end());
        for (std::uint16_t mode = 0; mode < GALAXY_INPUT_MODE_COUNT; ++mode) {
            auto const source = galaxy_input_mode_source(mode);
            auto const src_bytes = make_const_span(source);
            bytes.insert(bytes.end(), src_bytes.begin(), src_bytes.end());
        }
        add_raw(DESCRIPTOR_SIGNAL_SELECTOR, i, bytes);
    }

    // ---- LOCALE + STRINGS (Clause 7.2.11/7.2.12) ----
    // One "en-US" locale over the whole table; string n lives in STRINGS
    // descriptor n/7 at offset n%7, matching every loc() reference above.
    DescriptorLocale locale;
    locale.locale_identifier = AtdeccString{"en-US"};
    locale.number_of_strings = strings_count;
    locale.base_strings = 0;
    add_raw(DESCRIPTOR_LOCALE, 0, make_const_span(locale));
    for (std::uint16_t si = 0; si < strings_count; ++si) {
        DescriptorStrings str;
        str.descriptor_index = si;
        std::array<AtdeccString*, 7> slot{
            &str.string_0, &str.string_1, &str.string_2, &str.string_3, &str.string_4, &str.string_5, &str.string_6};
        for (std::size_t k = 0; k < slot.size(); ++k) {
            auto const at = std::size_t(si) * 7 + k;
            if (at < strings_.size()) {
                *slot[k] = AtdeccString{strings_[at]};
            }
        }
        add_raw(DESCRIPTOR_STRINGS, si, make_const_span(str));
    }

    // ---- blob ----
    nanoavb::AemBlobWriter writer;
    for (auto const& d : model.descriptors) {
        writer.add(d.configuration, d.descriptor_type, d.descriptor_index, d.bytes);
    }
    model.blob = writer.write();
    model.binding = binder_.build();
    return model;
}

}  // namespace

std::size_t GalaxyAemModel::count_of(std::uint16_t descriptor_type) const
{
    std::size_t n = 0;
    for (auto const& d : descriptors) {
        if (d.descriptor_type == descriptor_type) {
            ++n;
        }
    }
    return n;
}

// Meter-bank widths per model, folded from the product datasheets (see
// the struct's doc block). Sources: GALAXY 408 datasheet — "4 processed
// inputs selectable as analog, AES3 or AVB", "One SIM 3 bus port",
// "Processed outputs 1-8 on connectors 1-8, (9-16 AVB outputs only)",
// "Sparse 32 x 16 Summing Matrix"; GALAXY 816-AES3 datasheet — inputs
// A-H, "two SIM bus ports", outputs 1-8 AES3/AVB with 9-16 analog-capable.
// GX-816 is the live-verified row (LA Q1, fw 2.10.0).
constexpr GalaxyModelBanks GALAXY_MODEL_BANKS[] = {
    {"GX-408", 4, 4, 32, 16, 1, 8, GALAXY_PROXY_ENTITY_MODEL_ID_GX408},
    {"GX-816", 8, 8, 32, 16, 2, 16, GALAXY_PROXY_ENTITY_MODEL_ID_GX816},
    {"GX-816-AES3", 8, 8, 32, 16, 2, 8, GALAXY_PROXY_ENTITY_MODEL_ID_GX816AES3},
};

SignalSource galaxy_input_mode_source(std::uint16_t mode)
{
    return SignalSource{GALAXY_MODE_SIGNAL_TYPE, mode, 0};
}

std::optional<std::uint16_t> galaxy_input_mode_of(SignalSource const& source)
{
    if (source.signal_type.get() != GALAXY_MODE_SIGNAL_TYPE || source.signal_output.get() != 0 ||
        source.signal_index.get() >= GALAXY_INPUT_MODE_COUNT) {
        return std::nullopt;
    }
    return source.signal_index.get();
}

GalaxyModelBanks const& galaxy_model_banks(std::string_view model_string)
{
    for (auto const& banks : GALAXY_MODEL_BANKS) {
        if (model_string == banks.model_string) {
            return banks;
        }
    }
    return GALAXY_MODEL_BANKS[1];  // GX-816 fallback (incl. Bluehorn 816)
}

GalaxyAemModel build_galaxy_proxy_aem_model(GalaxyNamespace const& ns)
{
    return build_galaxy_proxy_aem_model(ns, galaxy_model_banks("GX-816"));
}

GalaxyAemModel build_galaxy_proxy_aem_model(GalaxyNamespace const& ns, GalaxyModelBanks const& banks)
{
    Generator gen(ns, banks);
    return gen.run();
}

}  // namespace statusbar::galaxy_proxy
