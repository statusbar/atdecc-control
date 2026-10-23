// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nadia_proxy/nadia_aem_model.hpp"

#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_aem_units.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/controlplane_devices/nadia_ascii.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace statusbar::nadia_proxy {

using namespace statusbar::atdecc::aem;
using controlplane::AemBindingBuilder;
using controlplane::AemRef;
using controlplane::ControlPointId;
using controlplane_devices::NadiaNamespace;
using controlplane_devices::NadiaShape;
using controlplane_devices::NadiaValueKind;
using ieee::Eui64;

namespace {

// ---- provisional vendor control types (JDKS OUI-36, bank 0x3) -----------

constexpr Eui64 vendor_type(std::uint8_t n)
{
    return Eui64{0x70, 0xB3, 0xD5, 0xED, 0xC3, 0x00, 0x00, n};
}

Eui64 const VT_SOLO = vendor_type(0x01);
Eui64 const VT_SOLO_SAFE = vendor_type(0x02);
Eui64 const VT_ISOLATE = vendor_type(0x03);
Eui64 const VT_NAME = vendor_type(0x04);
Eui64 const VT_ADDRESS = vendor_type(0x05);
Eui64 const VT_TOUCHED = vendor_type(0x06);
Eui64 const VT_BYPASS = vendor_type(0x07);  ///< 255 = bypassed (ENABLE's inverse)
Eui64 const VT_EQ_BAND = vendor_type(0x08);
Eui64 const VT_EQ_BAND_TYPE = vendor_type(0x09);
Eui64 const VT_ATTACK = vendor_type(0x0A);
Eui64 const VT_HOLD = vendor_type(0x0B);
Eui64 const VT_RELEASE = vendor_type(0x0C);
Eui64 const VT_THRESHOLD = vendor_type(0x0D);
Eui64 const VT_RATIO = vendor_type(0x0E);
Eui64 const VT_RANGE = vendor_type(0x0F);
Eui64 const VT_SYSTEM_HOLD = vendor_type(0x10);
Eui64 const VT_FAULT = vendor_type(0x11);
Eui64 const VT_STATUS_INFO = vendor_type(0x12);
Eui64 const VT_STATUS_LEVEL = vendor_type(0x13);
Eui64 const VT_CROSSPOINT_ISOLATE = vendor_type(0x14);
Eui64 const VT_USHAPING_BAND = vendor_type(0x15);
Eui64 const VT_REVERB_PARAM = vendor_type(0x16);
Eui64 const VT_GEOMETRY = vendor_type(0x17);
Eui64 const VT_SELECT = vendor_type(0x18);
Eui64 const VT_AGC_RATE = vendor_type(0x19);
Eui64 const VT_AGC_UNIT = vendor_type(0x1A);

constexpr std::uint16_t NO_LOCALIZED = 0xFFFF;
constexpr SignalSource NO_SIGNAL{0xFFFF, 0, 0};

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
constexpr ControlUnits UNIT_NS = make_units(-9, UNIT_CODE_SECONDS);

/// FNV-1a over the generation and the shape's counts.
std::uint32_t shape_hash(NadiaShape const& shape)
{
    std::uint32_t h = 2166136261u;
    auto mix = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 16777619u;
        }
    };
    mix(NADIA_PROXY_MODEL_GENERATION);
    mix(shape.inputs);
    mix(shape.buses);
    mix(shape.outputs);
    mix(shape.listens);
    mix(shape.vgroups);
    mix(shape.eq_bands);
    mix(shape.system ? 1u : 0u);
    mix(shape.vras);
    mix(shape.vras_inputs);
    mix(shape.vras_returns);
    mix(shape.vras_outputs);
    mix(shape.reflections);
    mix(shape.monitors);
    mix(shape.agcs);
    mix(shape.master ? 1u : 0u);
    return h;
}

Eui64 eui64_from(std::uint64_t v)
{
    return Eui64{
        std::uint8_t(v >> 56),
        std::uint8_t(v >> 48),
        std::uint8_t(v >> 40),
        std::uint8_t(v >> 32),
        std::uint8_t(v >> 24),
        std::uint8_t(v >> 16),
        std::uint8_t(v >> 8),
        std::uint8_t(v)};
}

/// The wire value type a point kind is served as.
std::uint16_t linear_type_of(NadiaValueKind kind)
{
    switch (kind) {
        case NadiaValueKind::boolean:
            return CONTROL_LINEAR_UINT8;
        case NadiaValueKind::nanoseconds:
            return CONTROL_LINEAR_INT64;
        case NadiaValueKind::integer:
            return CONTROL_LINEAR_INT32;
        case NadiaValueKind::text:
            return CONTROL_UTF8;
        case NadiaValueKind::decibels:
        case NadiaValueKind::hertz:
        case NadiaValueKind::ratio:
            break;
    }
    return CONTROL_LINEAR_FLOAT;
}

std::uint16_t array_type_of(NadiaValueKind kind)
{
    switch (linear_type_of(kind)) {
        case CONTROL_LINEAR_UINT8:
            return CONTROL_ARRAY_UINT8;
        case CONTROL_LINEAR_INT64:
            return CONTROL_ARRAY_INT64;
        case CONTROL_LINEAR_INT32:
            return CONTROL_ARRAY_INT32;
        default:
            return CONTROL_ARRAY_FLOAT;
    }
}

ControlUnits unit_of(NadiaValueKind kind)
{
    switch (kind) {
        case NadiaValueKind::decibels:
            return UNIT_DB;
        case NadiaValueKind::hertz:
            return UNIT_HZ;
        case NadiaValueKind::nanoseconds:
            return UNIT_NS;
        default:
            return UNIT_NONE;
    }
}

/// A point's authored format: range from the schema model (kind defaults
/// when unranged), step by kind, default 0 clamped into the range.
struct Format
{
    double min, max, step, def;
    ControlUnits unit;
};

Format format_of(NadiaNamespace const& ns, ControlPointId h)
{
    auto const kind = ns.kinds[h];
    auto const& m = *ns.model;
    Format f{0.0, 0.0, 1.0, 0.0, unit_of(kind)};
    if (m.min(h) < m.max(h)) {
        f.min = m.min(h);
        f.max = m.max(h);
    } else {
        switch (kind) {
            case NadiaValueKind::boolean:
                f.min = 0.0;
                f.max = 255.0;
                break;
            case NadiaValueKind::integer:
                f.min = -2147483648.0;
                f.max = 2147483647.0;
                break;
            case NadiaValueKind::nanoseconds:
                f.min = -9.2e18;
                f.max = 9.2e18;
                break;
            default:
                f.min = -1.0e9;
                f.max = 1.0e9;
                break;
        }
    }
    switch (kind) {
        case NadiaValueKind::boolean:
            f.min = 0.0;
            f.max = 255.0;
            f.step = 255.0;
            break;
        case NadiaValueKind::decibels:
            f.step = 0.1;
            break;
        case NadiaValueKind::ratio:
            f.step = 0.01;
            f.def = 1.0;
            break;
        case NadiaValueKind::hertz:
            f.def = 1000.0;
            break;
        default:
            break;
    }
    f.def = std::clamp(f.def, f.min, f.max);
    return f;
}

template <typename T>
void write_linear_entry(std::uint8_t* dst, Format const& f)
{
    LinearValueEntry<T> entry;
    entry.set_minimum(T(f.min));
    entry.set_maximum(T(f.max));
    entry.set_step(T(f.step));
    entry.set_default_value(T(f.def));
    entry.set_current(T(f.def));
    entry.unit = f.unit;
    entry.localized_string = NO_LOCALIZED;
    std::memcpy(dst, &entry, sizeof entry);
}

void write_linear_entry(std::uint16_t value_type, std::uint8_t* dst, Format const& f)
{
    switch (value_type) {
        case CONTROL_LINEAR_UINT8:
            return write_linear_entry<std::uint8_t>(dst, f);
        case CONTROL_LINEAR_INT32:
            return write_linear_entry<std::int32_t>(dst, f);
        case CONTROL_LINEAR_INT64:
            return write_linear_entry<std::int64_t>(dst, f);
        default:
            return write_linear_entry<float>(dst, f);
    }
}

template <typename T>
std::size_t put_field(std::uint8_t* dst, double value)
{
    std::array<std::uint8_t, sizeof(T)> bytes{};
    detail::encode_linear_field(T(value), bytes);
    std::memcpy(dst, bytes.data(), bytes.size());
    return bytes.size();
}

std::size_t put_field(std::uint16_t array_type, std::uint8_t* dst, double value)
{
    switch (array_type) {
        case CONTROL_ARRAY_UINT8:
            return put_field<std::uint8_t>(dst, value);
        case CONTROL_ARRAY_INT32:
            return put_field<std::int32_t>(dst, value);
        case CONTROL_ARRAY_INT64:
            return put_field<std::int64_t>(dst, value);
        default:
            return put_field<float>(dst, value);
    }
}

std::string indexed(std::string_view family, unsigned i)
{
    return "/" + std::string(family) + "/" + std::to_string(i);
}

class Generator
{
  public:
    explicit Generator(NadiaNamespace const& ns)
        : ns_(ns)
        , binder_(ns.size())
    {}

    NadiaAemModel run();

  private:
    ControlPointId resolve(std::string const& path) const
    {
        auto h = ns_.adaptor.resolve(path);
        assert(h.has_value());  // the generator and nadia_schema must agree on the catalogue
        return *h;
    }

    void bind_value(std::uint16_t value_index, ControlPointId h)
    {
        bool ok = binder_.bind(AemRef{0, DESCRIPTOR_CONTROL, next_control_, value_index}, h);
        assert(ok);
        (void)ok;
    }

    // -- localized strings (Clause 7.3.7), 7 per STRINGS descriptor --
    std::uint16_t loc(std::string_view name)
    {
        auto const n = strings_.size();
        assert(n / 7 < 0x1FFF);
        strings_.emplace_back(name);
        return std::uint16_t(((n / 7) << 3) | (n % 7));
    }

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

    std::uint16_t emit_control(DescriptorControl const& c)
    {
        auto view = make_const_span(c);
        controls_.emplace_back(view.begin(), view.begin() + long(c.wire_size()));
        return next_control_++;
    }

    /// One point as a single-value control in its kind's format.
    std::uint16_t add_point(std::string_view name, Eui64 const& type, std::string const& path)
    {
        auto const h = resolve(path);
        auto const kind = ns_.kinds[h];
        auto const read_only = ns_.model->read_only(h);
        auto const value_type = linear_type_of(kind);
        auto c = control_header(name, type, value_type, read_only, 1);
        if (value_type != CONTROL_UTF8) {
            write_linear_entry(value_type, c.value_details_bytes_mutable().data(), format_of(ns_, h));
        }
        bind_value(0, h);
        return emit_control(c);
    }

    /// Several float-kind points as one grouped LINEAR_FLOAT control.
    std::uint16_t add_group(std::string_view name, Eui64 const& type, std::initializer_list<std::string> paths)
    {
        auto c = control_header(name, type, CONTROL_LINEAR_FLOAT, false, std::uint16_t(paths.size()));
        auto trailer = c.value_details_bytes_mutable();
        std::size_t at = 0;
        std::uint16_t v = 0;
        for (auto const& path : paths) {
            auto const h = resolve(path);
            assert(linear_type_of(ns_.kinds[h]) == CONTROL_LINEAR_FLOAT);
            write_linear_entry<float>(trailer.data() + at, format_of(ns_, h));
            at += sizeof(LinearValueEntry<float>);
            bind_value(v++, h);
        }
        return emit_control(c);
    }

    /// A row of same-kind points as array controls, NADIA_ARRAY_CHUNK
    /// values each ("Name" when one chunk suffices, else "Name a-b").
    /// The array format (Table 7.12/7.13) is min, max, step, default,
    /// unit, string, then the current values; the row's format is its
    /// first point's.
    void add_array(std::string const& name, Eui64 const& type, std::vector<std::string> const& paths)
    {
        if (paths.empty()) {
            return;
        }
        auto const first = resolve(paths.front());
        auto const kind = ns_.kinds[first];
        auto const read_only = ns_.model->read_only(first);
        auto const value_type = array_type_of(kind);
        auto const fmt = format_of(ns_, first);
        for (std::size_t start = 0; start < paths.size(); start += NADIA_ARRAY_CHUNK) {
            auto const n = std::uint16_t(std::min<std::size_t>(NADIA_ARRAY_CHUNK, paths.size() - start));
            auto label = name;
            if (paths.size() > NADIA_ARRAY_CHUNK) {
                label += " " + std::to_string(start + 1) + "-" + std::to_string(start + n);
            }
            auto c = control_header(label, type, value_type, read_only, n);
            auto trailer = c.value_details_bytes_mutable();
            std::size_t at = 0;
            at += put_field(value_type, trailer.data() + at, fmt.min);
            at += put_field(value_type, trailer.data() + at, fmt.max);
            at += put_field(value_type, trailer.data() + at, fmt.step);
            at += put_field(value_type, trailer.data() + at, fmt.def);
            trailer[at++] = fmt.unit.multiplier;
            trailer[at++] = fmt.unit.code;
            trailer[at++] = 0xFF;  // localized_string = NONE
            trailer[at++] = 0xFF;
            for (std::uint16_t v = 0; v < n; ++v) {
                at += put_field(value_type, trailer.data() + at, fmt.def);
                bind_value(v, resolve(paths[start + v]));
            }
            assert(at == c.value_details_length());
            emit_control(c);
        }
    }

    void add_strip(char const* family, char const* display, unsigned i, bool dsp);
    void add_strip_at(std::string const& F, std::string const& disp, bool dsp, bool ushaping, std::string_view family_token);
    void add_vras_engine(unsigned e);
    void add_monitor(unsigned i);
    void add_agc(unsigned i);
    void add_master();
    void add_crosspoints(char const* family, char const* display, unsigned i, char const* dest, char const* dest_display);
    void add_system();

    NadiaNamespace const& ns_;
    AemBindingBuilder binder_;
    std::vector<std::vector<std::uint8_t>> controls_;
    std::uint16_t next_control_ = 0;

    struct Block
    {
        std::string name;
        std::uint16_t base;
        std::uint16_t count;
    };
    std::vector<Block> blocks_;
    std::vector<std::string> strings_;
};

void Generator::add_strip(char const* family, char const* display, unsigned i, bool dsp)
{
    add_strip_at(
        indexed(family, i),
        std::string(display) + " " + std::to_string(i),
        dsp,
        std::string_view(family) == "output",  // outputs carry U-Shaping
        family);
}

void Generator::add_strip_at(std::string const& F, std::string const& disp, bool dsp, bool ushaping, std::string_view family_token)
{
    auto const base = next_control_;
    auto p = [&](char const* leaf) { return F + leaf; };
    auto label = [&](std::string_view what) { return disp + " " + std::string(what); };

    add_point(label("Level"), CONTROL_TYPE_GAIN, p("/level"));
    add_point(label("Trim"), CONTROL_TYPE_GAIN, p("/trim"));
    add_point(label("Mute"), CONTROL_TYPE_MUTE, p("/mute"));
    add_point(label("Solo"), VT_SOLO, p("/solo"));
    add_point(label("Solo Safe"), VT_SOLO_SAFE, p("/solo/safe"));
    add_point(label("Isolate"), VT_ISOLATE, p("/isolate"));
    add_point(label("Name"), VT_NAME, p("/name"));
    add_point(label("Address"), VT_ADDRESS, p("/address"));
    add_point(label("Effective Level"), CONTROL_TYPE_GAIN, p("/effective/level"));
    add_point(label("Effective Mute"), CONTROL_TYPE_MUTE, p("/effective/mute"));
    add_point(label("Effective Solo"), VT_SOLO, p("/effective/solo"));
    add_point(label("Touched"), VT_TOUCHED, p("/touched"));
    if (dsp) {
        add_point(label("Invert"), CONTROL_TYPE_INVERT, p("/invert"));
        add_point(label("Delay"), CONTROL_TYPE_DELAY, p("/delay"));
        add_point(label("Delay Bypass"), VT_BYPASS, p("/delay/bypass"));
        add_point(label("Delay Trim"), CONTROL_TYPE_DELAY, p("/delay/trim"));
        add_point(label("Delay Trim Bypass"), VT_BYPASS, p("/delay/trim/bypass"));
        add_point(label("Effective Delay"), CONTROL_TYPE_DELAY, p("/effective/delay"));
        add_point(label("Meter"), CONTROL_TYPE_AUDIO_METERS, p("/meter"));
        if (family_token == "input") {
            add_point(label("PreFader Meter"), CONTROL_TYPE_AUDIO_METERS, p("/prefader/meter"));
            add_point(label("PostFader Meter"), CONTROL_TYPE_AUDIO_METERS, p("/postfader/meter"));
        } else if (family_token == "bus") {
            add_point(label("PreFader Meter"), CONTROL_TYPE_AUDIO_METERS, p("/prefader/meter"));
        }
        add_point(label("EQ Bypass"), VT_BYPASS, p("/eq/bypass"));
        add_point(label("Parametric Bypass"), VT_BYPASS, p("/parametric/bypass"));
        for (unsigned b = 1; b <= ns_.shape.eq_bands; ++b) {
            auto const B = F + "/parametric/" + std::to_string(b);
            auto const band = label("Parametric " + std::to_string(b));
            add_group(band, VT_EQ_BAND, {B + "/frequency", B + "/level", B + "/q"});
            add_point(band + " Bypass", VT_BYPASS, B + "/bandbypass");
            add_point(band + " Type", VT_EQ_BAND_TYPE, B + "/type");
            add_point(band + " Name", VT_NAME, B + "/name");
        }
        if (ushaping) {
            add_point(label("UShaping Bypass"), VT_BYPASS, p("/ushaping/bypass"));
            for (unsigned b = 1; b <= 4; ++b) {
                auto const U = F + "/ushaping/" + std::to_string(b);
                add_point(label("UShaping " + std::to_string(b) + " Frequency"), VT_USHAPING_BAND, U + "/frequency");
                add_point(label("UShaping " + std::to_string(b) + " Slope"), VT_USHAPING_BAND, U + "/slope");
            }
            for (unsigned b = 1; b <= 5; ++b) {
                auto const U = F + "/ushaping/" + std::to_string(b);
                add_point(label("UShaping " + std::to_string(b) + " Level"), VT_USHAPING_BAND, U + "/level");
            }
        }
        add_point(label("Dynamics Bypass"), VT_BYPASS, p("/dynamics/bypass"));
        add_point(label("Dynamics Makeup Gain"), CONTROL_TYPE_GAIN, p("/dynamics/makeupgain"));
        struct Section
        {
            char const* token;
            char const* display;
        };
        for (auto const& s :
             {Section{"noisegate", "NoiseGate"},
              Section{"compressor", "Compressor"},
              Section{"limiter", "Limiter"},
              Section{"expander", "Expander"}}) {
            auto const S = F + "/dynamics/" + s.token;
            auto const sec = label(std::string("Dynamics ") + s.display);
            add_point(sec + " Attack", VT_ATTACK, S + "/attack");
            add_point(sec + " Bypass", VT_BYPASS, S + "/bypass");
            add_point(sec + " Effective Bypass", VT_BYPASS, S + "/effective/bypass");
            add_point(sec + " Hold", VT_HOLD, S + "/hold");
            add_point(sec + " Release", VT_RELEASE, S + "/release");
            add_point(sec + " Threshold", VT_THRESHOLD, S + "/threshold");
            if (std::string_view(s.token) == "noisegate") {
                add_point(sec + " Range", VT_RANGE, S + "/range");
            } else {
                add_point(sec + " Ratio", VT_RATIO, S + "/ratio");
            }
        }
    }
    if (family_token == "input" && ns_.shape.buses > 0) {
        add_crosspoints("input", "Input", unsigned(std::atoi(F.c_str() + F.rfind('/') + 1)), "bus", "Bus");
    } else if (family_token == "bus" && ns_.shape.outputs > 0) {
        add_crosspoints("bus", "Bus", unsigned(std::atoi(F.c_str() + F.rfind('/') + 1)), "output", "Output");
    }
    blocks_.push_back({disp, base, std::uint16_t(next_control_ - base)});
}

void Generator::add_crosspoints(char const* family, char const* display, unsigned i, char const* dest, char const* dest_display)
{
    auto const F = indexed(family, i) + "/" + dest;
    auto const n = std::string_view(dest) == "bus" ? ns_.shape.buses : ns_.shape.outputs;
    auto label = [&](std::string_view what) {
        return std::string(display) + " " + std::to_string(i) + " " + dest_display + " " + std::string(what);
    };
    auto row = [&](char const* leaf) {
        std::vector<std::string> paths;
        for (unsigned d = 1; d <= n; ++d) {
            paths.push_back(F + "/" + std::to_string(d) + leaf);
        }
        return paths;
    };

    add_point(label("Isolate"), VT_CROSSPOINT_ISOLATE, F + "/isolate");
    add_array(label("Levels"), CONTROL_TYPE_GAIN, row("/level"));
    add_array(label("Trims"), CONTROL_TYPE_GAIN, row("/trim"));
    if (std::string_view(dest) == "output") {
        add_array(label("Delays"), CONTROL_TYPE_DELAY, row("/delay"));
        add_array(label("Delay Bypasses"), VT_BYPASS, row("/delay/bypass"));
        add_array(label("Delay Trims"), CONTROL_TYPE_DELAY, row("/delay/trim"));
        add_array(label("Delay Trim Bypasses"), VT_BYPASS, row("/delay/trim/bypass"));
    }
    add_array(label("Effective Levels"), CONTROL_TYPE_GAIN, row("/effective/level"));
    if (std::string_view(dest) == "output") {
        add_array(label("Effective Delays"), CONTROL_TYPE_DELAY, row("/effective/delay"));
    }
}

void Generator::add_vras_engine(unsigned e)
{
    auto const base = next_control_;
    auto const E = "/vras/" + std::to_string(e);
    auto const L = "VRAS " + std::to_string(e) + " ";
    add_point(L + "Name", VT_NAME, E + "/name");
    add_point(L + "Reverb Duration", VT_REVERB_PARAM, E + "/reverb/duration");
    add_point(L + "Reverb Level", CONTROL_TYPE_GAIN, E + "/reverb/level");
    add_point(L + "Reverb Input Geometry", VT_GEOMETRY, E + "/reverb/input/geometry");
    add_point(L + "Reverb Input Offset", VT_GEOMETRY, E + "/reverb/input/offset");
    add_point(L + "Reverb Damping Type", VT_REVERB_PARAM, E + "/reverb/damping/type");
    add_point(L + "Reverb Damping Parametric Status", VT_STATUS_INFO, E + "/reverb/damping/parametric/status");
    add_point(L + "Reverb Diffuser Count", VT_REVERB_PARAM, E + "/reverb/diffuser/count");
    add_point(L + "Reverb Diffusion Count", VT_REVERB_PARAM, E + "/reverb/diffusion/count");
    add_point(L + "Reverb Diffusion Enable", VT_BYPASS, E + "/reverb/diffusion/enable");
    for (unsigned r = 1; r <= 2; ++r) {
        auto const R = E + "/reverb/" + std::to_string(r);
        auto const rl = L + "Reverb " + std::to_string(r) + " ";
        add_point(rl + "ShapeFactor", VT_REVERB_PARAM, R + "/shapefactor");
        add_point(rl + "DeltaMax", VT_REVERB_PARAM, R + "/deltamax");
        add_point(rl + "Diffuser ShapeFactor", VT_REVERB_PARAM, R + "/diffuser/shapefactor");
        add_point(rl + "Diffuser Delay Divisor", VT_REVERB_PARAM, R + "/diffuser/delay/divisor");
        add_point(rl + "Diffuser Level", CONTROL_TYPE_GAIN, R + "/diffuser/level");
        add_point(rl + "Damping Frequency", VT_EQ_BAND, R + "/damping/frequency");
        add_point(rl + "Damping Level", CONTROL_TYPE_GAIN, R + "/damping/level");
    }
    for (unsigned r = 1; r <= 5; ++r) {
        auto const R = E + "/reverb/" + std::to_string(r) + "/damping/parametric";
        auto const rl = L + "Reverb " + std::to_string(r) + " Damping Parametric ";
        add_point(rl + "BandBypass", VT_BYPASS, R + "/bandbypass");
        add_point(rl + "Frequency", VT_EQ_BAND, R + "/frequency");
        add_point(rl + "Level", CONTROL_TYPE_GAIN, R + "/level");
        add_point(rl + "Q", VT_RATIO, R + "/q");
        add_point(rl + "Type", VT_EQ_BAND_TYPE, R + "/type");
        add_point(rl + "Beta", VT_REVERB_PARAM, R + "/beta");
    }
    for (unsigned r = 1; r <= ns_.shape.reflections; ++r) {
        auto const R = E + "/earlyreflection/" + std::to_string(r);
        auto const rl = L + "EarlyReflection " + std::to_string(r) + " ";
        add_point(rl + "Level", CONTROL_TYPE_GAIN, R + "/level");
        add_point(rl + "Mute", CONTROL_TYPE_MUTE, R + "/mute");
        add_point(rl + "Delay", CONTROL_TYPE_DELAY, R + "/delay");
    }
    add_point(L + "EarlyReflection Attenuation", CONTROL_TYPE_GAIN, E + "/earlyreflection/attenuation");
    add_point(L + "EarlyReflection Frequency", VT_EQ_BAND, E + "/earlyreflection/frequency");
    add_point(L + "EarlyReflection Squeeze", VT_REVERB_PARAM, E + "/earlyreflection/squeeze");
    add_point(L + "EarlyReflection Short Spread", VT_REVERB_PARAM, E + "/earlyreflection/short/spread");
    add_point(L + "EarlyReflection Long ShapeFactor", VT_REVERB_PARAM, E + "/earlyreflection/long/shapefactor");
    add_point(L + "EarlyReflection Maximum Delay", CONTROL_TYPE_DELAY, E + "/earlyreflection/maximum/delay");
    add_point(L + "EarlyReflection Input Geometry", VT_GEOMETRY, E + "/earlyreflection/input/geometry");
    add_point(L + "EarlyReflection Output Geometry", VT_GEOMETRY, E + "/earlyreflection/output/geometry");
    blocks_.push_back({"VRAS " + std::to_string(e) + " Engine", base, std::uint16_t(next_control_ - base)});
}

void Generator::add_monitor(unsigned i)
{
    auto const base = next_control_;
    auto const F = "/monitor/" + std::to_string(i);
    auto const L = "Monitor " + std::to_string(i) + " ";
    add_point(L + "Name", VT_NAME, F + "/name");
    add_point(L + "Isolate", VT_ISOLATE, F + "/isolate");
    add_point(L + "AGC Unit", VT_AGC_UNIT, F + "/agc/unit");
    add_point(L + "Dynamics Makeup Gain", CONTROL_TYPE_GAIN, F + "/dynamics/makeupgain");
    add_point(L + "Dynamics Compressor Attack", VT_ATTACK, F + "/dynamics/compressor/attack");
    add_point(L + "Dynamics Compressor Bypass", VT_BYPASS, F + "/dynamics/compressor/bypass");
    add_point(L + "Dynamics Compressor Hold", VT_HOLD, F + "/dynamics/compressor/hold");
    add_point(L + "Dynamics Compressor Ratio", VT_RATIO, F + "/dynamics/compressor/ratio");
    add_point(L + "Dynamics Compressor Release", VT_RELEASE, F + "/dynamics/compressor/release");
    add_point(L + "Dynamics Compressor Threshold", VT_THRESHOLD, F + "/dynamics/compressor/threshold");
    blocks_.push_back({"Monitor " + std::to_string(i), base, std::uint16_t(next_control_ - base)});
}

void Generator::add_agc(unsigned i)
{
    auto const base = next_control_;
    auto const F = "/agc/" + std::to_string(i);
    auto const L = "AGC " + std::to_string(i) + " ";
    add_point(L + "Name", VT_NAME, F + "/name");
    add_point(L + "Bypass", VT_BYPASS, F + "/bypass");
    add_point(L + "Isolate", VT_ISOLATE, F + "/isolate");
    add_point(L + "Threshold", VT_THRESHOLD, F + "/threshold");
    add_point(L + "Up Rate", VT_AGC_RATE, F + "/up/rate");
    add_point(L + "Down Rate", VT_AGC_RATE, F + "/down/rate");
    add_point(L + "Maximum Gain", CONTROL_TYPE_GAIN, F + "/maximum/gain");
    add_point(L + "EQ Bypass", VT_BYPASS, F + "/eq/bypass");
    add_point(L + "Parametric Bypass", VT_BYPASS, F + "/parametric/bypass");
    add_point(L + "Parametric Status", VT_STATUS_INFO, F + "/parametric/status");
    for (unsigned b = 1; b <= ns_.shape.eq_bands; ++b) {
        auto const B = F + "/parametric/" + std::to_string(b);
        auto const band = L + "Parametric " + std::to_string(b);
        add_group(band, VT_EQ_BAND, {B + "/frequency", B + "/level", B + "/q"});
        add_point(band + " Bypass", VT_BYPASS, B + "/bandbypass");
        add_point(band + " Type", VT_EQ_BAND_TYPE, B + "/type");
        add_point(band + " Name", VT_NAME, B + "/name");
    }
    blocks_.push_back({"AGC " + std::to_string(i), base, std::uint16_t(next_control_ - base)});
}

void Generator::add_master()
{
    auto const base = next_control_;
    struct Entry
    {
        char const* display;
        char const* path;
    };
    for (auto const& m :
         {Entry{"Master System Select", "/master/system/select"},
          Entry{"Master Input Select", "/master/input/select"},
          Entry{"Master Input Isolate", "/master/input/isolate"},
          Entry{"Master Output Select", "/master/output/select"},
          Entry{"Master Output Solo", "/master/output/solo"},
          Entry{"Master Output Mute", "/master/output/mute"},
          Entry{"Master Output Isolate", "/master/output/isolate"},
          Entry{"Master Output Invert", "/master/output/invert"},
          Entry{"Master Output Touched", "/master/output/touched"},
          Entry{"Master Output EQ Bypass", "/master/output/eq/bypass"},
          Entry{"Master Output Parametric Bypass", "/master/output/parametric/bypass"},
          Entry{"Master Listen Select", "/master/listen/select"},
          Entry{"Master Listen Solo", "/master/listen/solo"},
          Entry{"Master Listen Mute", "/master/listen/mute"},
          Entry{"Master Listen Isolate", "/master/listen/isolate"},
          Entry{"Master Listen Invert", "/master/listen/invert"},
          Entry{"Master Listen Touched", "/master/listen/touched"},
          Entry{"Master Listen EQ Bypass", "/master/listen/eq/bypass"},
          Entry{"Master Listen Parametric Bypass", "/master/listen/parametric/bypass"},
          Entry{"Master VGroup Select", "/master/vgroup/select"},
          Entry{"Master VGroup Solo", "/master/vgroup/solo"},
          Entry{"Master VGroup Mute", "/master/vgroup/mute"},
          Entry{"Master VGroup Isolate", "/master/vgroup/isolate"},
          Entry{"Master VGroup Touched", "/master/vgroup/touched"},
          Entry{"Master Monitor Select", "/master/monitor/select"},
          Entry{"Master Monitor Isolate", "/master/monitor/isolate"},
          Entry{"Master AGC Select", "/master/agc/select"},
          Entry{"Master AGC Isolate", "/master/agc/isolate"},
          Entry{"Master AGC EQ Bypass", "/master/agc/eq/bypass"},
          Entry{"Master AGC Parametric Bypass", "/master/agc/parametric/bypass"},
          Entry{"Master VRAS Select", "/master/vras/select"},
          Entry{"Master Row Select", "/master/row/select"},
          Entry{"Master Column Select", "/master/column/select"},
          Entry{"Master Script Select", "/master/script/select"},
          Entry{"Master CueListPlayer Select", "/master/cuelistplayer/select"},
          Entry{"Master WildTracks Select", "/master/wildtracks/select"},
          Entry{"Master WildTracks Channel Select", "/master/wildtracks/channel/select"},
          Entry{"Master WildTracks Deck Select", "/master/wildtracks/deck/select"}}) {
        add_point(m.display, VT_SELECT, m.path);
    }
    for (unsigned e = 1; e <= ns_.shape.vras; ++e) {
        for (char const* sub : {"input", "return", "output"}) {
            auto const B = "/master/vras/" + std::to_string(e) + "/" + sub;
            auto sub_display = std::string(1, char(std::toupper(sub[0]))) + (sub + 1);
            auto const L = "Master VRAS " + std::to_string(e) + " " + sub_display + " ";
            add_point(L + "Select", VT_SELECT, B + "/select");
            add_point(L + "Solo", VT_SOLO, B + "/solo");
            add_point(L + "Mute", CONTROL_TYPE_MUTE, B + "/mute");
            add_point(L + "Isolate", VT_ISOLATE, B + "/isolate");
            add_point(L + "Invert", CONTROL_TYPE_INVERT, B + "/invert");
            add_point(L + "EQ Bypass", VT_BYPASS, B + "/eq/bypass");
            add_point(L + "Parametric Bypass", VT_BYPASS, B + "/parametric/bypass");
        }
    }
    blocks_.push_back({"Master", base, std::uint16_t(next_control_ - base)});
}

void Generator::add_system()
{
    auto const base = next_control_;
    add_point("System Level", CONTROL_TYPE_GAIN, "/system/level");
    add_point("System Trim", CONTROL_TYPE_GAIN, "/system/trim");
    add_point("System Mute", CONTROL_TYPE_MUTE, "/system/mute");
    add_point("System Hold", VT_SYSTEM_HOLD, "/system/hold");
    add_point("System Isolate", VT_ISOLATE, "/system/isolate");
    add_point("System Touched", VT_TOUCHED, "/system/touched");
    add_point("System Meter Fault", VT_FAULT, "/system/meter/fault");
    add_point("System Status Info", VT_STATUS_INFO, "/system/status/info");
    add_point("System Status Level", VT_STATUS_LEVEL, "/system/status/level");
    blocks_.push_back({"System", base, std::uint16_t(next_control_ - base)});
}

NadiaAemModel Generator::run()
{
    // Identify first (customary index 0), then the strips in catalogue
    // family order, then the System master; CONTROL_BLOCKs are contiguous
    // control ranges, so a strip's controls are emitted together.
    {
        auto c = control_header("Identify", CONTROL_TYPE_IDENTIFY, CONTROL_LINEAR_UINT8, false, 1);
        write_linear_entry<std::uint8_t>(c.value_details_bytes_mutable().data(), Format{0.0, 255.0, 255.0, 0.0, UNIT_NONE});
        emit_control(c);
    }
    auto const& shape = ns_.shape;
    for (unsigned i = 1; i <= shape.inputs; ++i) {
        add_strip("input", "Input", i, true);
    }
    for (unsigned i = 1; i <= shape.buses; ++i) {
        add_strip("bus", "Bus", i, true);
    }
    for (unsigned i = 1; i <= shape.outputs; ++i) {
        add_strip("output", "Output", i, true);
    }
    for (unsigned i = 1; i <= shape.listens; ++i) {
        add_strip("listen", "Listen", i, true);
    }
    for (unsigned i = 1; i <= shape.vgroups; ++i) {
        add_strip("vgroup", "VGroup", i, false);
    }
    for (unsigned e = 1; e <= shape.vras; ++e) {
        auto const E = "/vras/" + std::to_string(e);
        auto const L = "VRAS " + std::to_string(e) + " ";
        for (unsigned i = 1; i <= shape.vras_inputs; ++i) {
            add_strip_at(E + "/input/" + std::to_string(i), L + "Input " + std::to_string(i), true, false, "");
        }
        for (unsigned i = 1; i <= shape.vras_returns; ++i) {
            add_strip_at(E + "/return/" + std::to_string(i), L + "Return " + std::to_string(i), true, true, "");
        }
        for (unsigned i = 1; i <= shape.vras_outputs; ++i) {
            add_strip_at(E + "/output/" + std::to_string(i), L + "Output " + std::to_string(i), true, false, "");
        }
        add_vras_engine(e);
    }
    for (unsigned i = 1; i <= shape.monitors; ++i) {
        add_monitor(i);
    }
    for (unsigned i = 1; i <= shape.agcs; ++i) {
        add_agc(i);
    }
    if (shape.master) {
        add_master();
    }

    if (shape.system) {
        add_system();
    }

    NadiaAemModel model;
    model.entity_model_id = nadia_proxy_entity_model_id(shape);
    auto add_raw = [&](std::uint16_t type, std::uint16_t index, std::span<std::uint8_t const> bytes) {
        model.descriptors.push_back({0, type, index, {bytes.begin(), bytes.end()}});
    };

    auto const vendor_loc = loc("Statusbar");
    auto const model_loc = loc("Nadia CP Proxy");
    auto const config_loc = loc("Nadia CP Proxy");
    auto const unit_loc = loc("Nadia Mixer");
    auto const avb_if_loc = loc("Ethernet");
    std::vector<std::uint16_t> block_locs;
    block_locs.reserve(blocks_.size());
    for (auto const& b : blocks_) {
        block_locs.push_back(loc(b.name));
    }
    auto const strings_count = std::uint16_t((strings_.size() + 6) / 7);

    // ---- ENTITY ----
    DescriptorEntity entity;
    entity.entity_id = Eui64{};  // patched at runtime from the Nadia's id
    entity.entity_model_id = eui64_from(model.entity_model_id);
    entity.entity_capabilities = atdecc::entity_capabilities::AEM_SUPPORTED;
    entity.entity_name = AtdeccString{"Nadia CP Proxy"};
    entity.vendor_name_string = vendor_loc;
    entity.model_name_string = model_loc;
    entity.firmware_version = AtdeccString{STATUSBAR_ATDECC_CONTROL_VERSION};
    entity.group_name = AtdeccString{""};
    entity.serial_number = AtdeccString{""};
    entity.configurations_count = 1;
    entity.current_configuration = 0;
    add_raw(DESCRIPTOR_ENTITY, 0, make_const_span(entity));

    // ---- CONFIGURATION ----
    DescriptorConfiguration config;
    config.object_name = AtdeccString{"Nadia CP Proxy"};
    config.localized_description = config_loc;
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_AUDIO_UNIT), 1});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_AVB_INTERFACE), 1});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_CONTROL), std::uint16_t(controls_.size())});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_CONTROL_BLOCK), std::uint16_t(blocks_.size())});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_LOCALE), 1});
    (void)config.push_descriptor_count({std::uint16_t(DESCRIPTOR_STRINGS), strings_count});
    add_raw(DESCRIPTOR_CONFIGURATION, 0, std::span(make_const_span(config)).first(config.wire_size()));

    // ---- AUDIO_UNIT ----
    DescriptorAudioUnit unit;
    unit.object_name = AtdeccString{"Nadia Mixer"};
    unit.localized_description = unit_loc;
    unit.number_of_controls = std::uint16_t(controls_.size());
    unit.base_control = 0;
    unit.number_of_control_blocks = std::uint16_t(blocks_.size());
    unit.base_control_block = 0;
    unit.current_sampling_rate = 0x0000BB80;  // 48 kHz, pull 0
    (void)unit.push_sampling_rate(ieee::quadlet_t{0x0000BB80});
    add_raw(DESCRIPTOR_AUDIO_UNIT, 0, std::span(make_const_span(unit)).first(unit.wire_size()));

    // ---- AVB_INTERFACE (JDKS experimental MAC; the runtime patches the
    // real NIC MAC and gPTP clock identity) ----
    DescriptorAvbInterface avb_if;
    avb_if.object_name = AtdeccString{"Ethernet"};
    avb_if.localized_description = avb_if_loc;
    avb_if.mac_address = ieee::Eui48{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0xF0};
    avb_if.clock_identity = Eui64{0x70, 0xB3, 0xD5, 0xFF, 0xFE, 0xED, 0xCF, 0xF0};
    add_raw(DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(avb_if));

    // ---- CONTROLs ----
    for (std::uint16_t i = 0; i < controls_.size(); ++i) {
        add_raw(DESCRIPTOR_CONTROL, i, controls_[i]);
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

    // ---- LOCALE + STRINGS ----
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

    nanoavb::AemBlobWriter writer;
    for (auto const& d : model.descriptors) {
        writer.add(d.configuration, d.descriptor_type, d.descriptor_index, d.bytes);
    }
    model.blob = writer.write();
    model.binding = binder_.build();
    assert(model.binding.bound_count() == ns_.size());  // every namespace point is served
    return model;
}

}  // namespace

std::uint64_t nadia_proxy_entity_model_id(NadiaShape const& shape)
{
    return 0x70B3D5EDC3800000ull | (shape_hash(shape) & 0x7FFFFFull);
}

std::size_t NadiaAemModel::count_of(std::uint16_t descriptor_type) const
{
    std::size_t n = 0;
    for (auto const& d : descriptors) {
        if (d.descriptor_type == descriptor_type) {
            ++n;
        }
    }
    return n;
}

NadiaAemModel build_nadia_proxy_aem_model(NadiaNamespace const& ns)
{
    Generator gen(ns);
    return gen.run();
}

}  // namespace statusbar::nadia_proxy
