// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane_devices/nadia_schema.hpp"

#include "statusbar/controlplane_devices/nadia_ascii.hpp"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace statusbar::controlplane_devices {

using controlplane::ControlPointId;

namespace {

using Kind = NadiaValueKind;

constexpr double DB_FLOOR = -90.0;
constexpr double DB_CEIL = 10.0;
constexpr double METER_FLOOR = -110.0;
constexpr double DELAY_MAX_NS = 500'000'000.0;
constexpr double DYN_TIME_MIN_NS = 200'000.0;
constexpr double DYN_TIME_MAX_NS = 5'000'000'000.0;

/// Expands one catalogue pattern. The template holds one "{}" per index;
/// counts give each index's population (outer index first). A zero count
/// drops the pattern; the whole population also becomes one ranged
/// command ("input 1-16 parametric 1-8 level").
struct Builder
{
    std::vector<NadiaPoint> points;
    std::vector<NadiaPatternCommand> patterns;

    void add(std::string_view tmpl, std::vector<std::uint16_t> const& counts, Kind kind, bool read_only, double min, double max)
    {
        std::vector<std::uint16_t> const& dims = counts;
        for (auto n : dims) {
            if (n == 0) {
                return;
            }
        }
        NadiaPatternCommand cmd;
        cmd.first = points.size();
        std::string ranged;
        std::size_t dim = 0;
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl.substr(i, 2) == "{}") {
                auto n = dims.at(dim++);
                ranged += n == 1 ? "1" : "1-" + std::to_string(n);
                ++i;
            } else {
                ranged.push_back(tmpl[i]);
            }
        }
        assert(dim == dims.size());
        cmd.addresses = nadia_command_address(ranged);
        expand(tmpl, dims, 0, {}, kind, read_only, min, max);
        cmd.count = points.size() - cmd.first;
        patterns.push_back(std::move(cmd));
    }

    void add(std::string_view tmpl, std::vector<std::uint16_t> const& counts, Kind kind, bool read_only = false)
    {
        add(tmpl, counts, kind, read_only, 0.0, 0.0);
    }

  private:
    void expand(
        std::string_view tmpl,
        std::vector<std::uint16_t> const& dims,
        std::size_t dim,
        std::vector<std::uint16_t> indices,
        Kind kind,
        bool read_only,
        double min,
        double max)
    {
        if (dim == dims.size()) {
            NadiaPoint p;
            std::size_t next = 0;
            for (std::size_t i = 0; i < tmpl.size(); ++i) {
                if (tmpl.substr(i, 2) == "{}") {
                    p.path += std::to_string(indices.at(next++));
                    ++i;
                } else {
                    p.path.push_back(tmpl[i]);
                }
            }
            p.kind = kind;
            p.read_only = read_only;
            p.min = min;
            p.max = max;
            points.push_back(std::move(p));
            return;
        }
        for (std::uint16_t i = 1; i <= dims[dim]; ++i) {
            indices.push_back(i);
            expand(tmpl, dims, dim + 1, indices, kind, read_only, min, max);
            indices.pop_back();
        }
    }
};

struct Family
{
    std::string prefix;                 ///< path up to the strip index, e.g. "/output" or "/vras/{}/return"
    std::vector<std::uint16_t> counts;  ///< one population per '{}' in prefix, then the strip count
    bool strip;                         ///< carries the DSP strip (invert/delay/EQ/dynamics/meter)
    bool ushaping;                      ///< carries the 5-band U-Shaping EQ (outputs, VRAS returns)
};

std::vector<std::uint16_t> cat(std::vector<std::uint16_t> counts, std::uint16_t extra)
{
    counts.push_back(extra);
    return counts;
}

void add_strip(Builder& b, Family const& f, std::uint16_t bands)
{
    std::string const F = f.prefix + "/{}";
    auto path = [&](char const* leaf) { return F + leaf; };
    auto const& n = f.counts;

    b.add(path("/level"), n, Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add(path("/trim"), n, Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add(path("/mute"), n, Kind::boolean);
    b.add(path("/solo"), n, Kind::boolean);
    b.add(path("/solo/safe"), n, Kind::boolean);
    b.add(path("/isolate"), n, Kind::boolean);
    b.add(path("/name"), n, Kind::text);
    b.add(path("/address"), n, Kind::text, true);
    b.add(path("/effective/level"), n, Kind::decibels, true, DB_FLOOR, DB_CEIL);
    b.add(path("/effective/mute"), n, Kind::boolean, true);
    b.add(path("/effective/solo"), n, Kind::boolean, true);
    b.add(path("/touched"), n, Kind::boolean, true);
    if (!f.strip) {
        return;
    }
    b.add(path("/invert"), n, Kind::boolean);
    b.add(path("/delay"), n, Kind::nanoseconds, false, 0.0, DELAY_MAX_NS);
    b.add(path("/delay/bypass"), n, Kind::boolean);
    b.add(path("/delay/trim"), n, Kind::nanoseconds, false, -DELAY_MAX_NS, DELAY_MAX_NS);
    b.add(path("/delay/trim/bypass"), n, Kind::boolean);
    b.add(path("/effective/delay"), n, Kind::nanoseconds, true, 0.0, DELAY_MAX_NS);
    b.add(path("/meter"), n, Kind::decibels, true, METER_FLOOR, 0.0);
    b.add(path("/eq/bypass"), n, Kind::boolean);
    b.add(path("/parametric/bypass"), n, Kind::boolean);
    b.add(path("/parametric/{}/bandbypass"), cat(n, bands), Kind::boolean);
    b.add(path("/parametric/{}/frequency"), cat(n, bands), Kind::hertz, false, 10.0, 20000.0);
    b.add(path("/parametric/{}/level"), cat(n, bands), Kind::decibels, false, -18.0, 18.0);
    b.add(path("/parametric/{}/q"), cat(n, bands), Kind::ratio, false, 0.5, 10.0);
    b.add(path("/parametric/{}/type"), cat(n, bands), Kind::integer, false, 3.0, 10.0);
    b.add(path("/parametric/{}/name"), cat(n, bands), Kind::text);
    if (f.ushaping) {
        b.add(path("/ushaping/bypass"), n, Kind::boolean);
        b.add(path("/ushaping/{}/frequency"), cat(n, 4), Kind::hertz, false, 10.0, 20000.0);
        b.add(path("/ushaping/{}/slope"), cat(n, 4), Kind::integer, false, 1.0, 4.0);
        b.add(path("/ushaping/{}/level"), cat(n, 5), Kind::decibels, false, -18.0, 18.0);
    }
    b.add(path("/dynamics/bypass"), n, Kind::boolean);
    b.add(path("/dynamics/makeupgain"), n, Kind::decibels, false, DB_FLOOR, 50.0);
    for (char const* band : {"noisegate", "compressor", "limiter", "expander"}) {
        std::string const B = F + "/dynamics/" + band;
        b.add(B + "/attack", n, Kind::nanoseconds, false, DYN_TIME_MIN_NS, DYN_TIME_MAX_NS);
        b.add(B + "/bypass", n, Kind::boolean);
        b.add(B + "/effective/bypass", n, Kind::boolean, true);
        b.add(B + "/hold", n, Kind::nanoseconds, false, 0.0, DYN_TIME_MAX_NS);
        b.add(B + "/release", n, Kind::nanoseconds, false, DYN_TIME_MIN_NS, DYN_TIME_MAX_NS);
        b.add(B + "/threshold", n, Kind::decibels, false, DB_FLOOR, DB_CEIL);
        std::string_view const band_name = band;
        if (band_name == "noisegate") {
            b.add(B + "/range", n, Kind::decibels, false, DB_FLOOR, DB_CEIL);
        } else if (band_name == "expander") {
            b.add(B + "/ratio", n, Kind::ratio, false, 0.1, 1.0);
        } else {
            b.add(B + "/ratio", n, Kind::ratio, false, 1.0, 20.0);
        }
    }
}

Builder expand_catalogue(NadiaShape const& shape)
{
    Builder b;
    Family const families[] = {
        {"/input", {shape.inputs}, true, false},
        {"/bus", {shape.buses}, true, false},
        {"/output", {shape.outputs}, true, true},
        {"/listen", {shape.listens}, true, false},
        {"/vgroup", {shape.vgroups}, false, false},
        // VRAS room engines carry full strips of their own.
        {"/vras/{}/input", {shape.vras, shape.vras_inputs}, true, false},
        {"/vras/{}/return", {shape.vras, shape.vras_returns}, true, true},
        {"/vras/{}/output", {shape.vras, shape.vras_outputs}, true, false},
    };
    for (auto const& f : families) {
        add_strip(b, f, shape.eq_bands);
    }

    // VRAS engine blocks: name, the two reverb engines with their damping
    // parametric (bands 3-5 on the RZ; 1-2 answer empty), and the early
    // reflections.
    std::vector<std::uint16_t> const V{shape.vras};
    b.add("/vras/{}/name", V, Kind::text);
    b.add("/vras/{}/reverb/duration", V, Kind::ratio, false, 0.0, 60.0);
    b.add("/vras/{}/reverb/level", V, Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add("/vras/{}/reverb/input/geometry", V, Kind::integer);
    b.add("/vras/{}/reverb/input/offset", V, Kind::integer);
    b.add("/vras/{}/reverb/damping/type", V, Kind::integer);
    b.add("/vras/{}/reverb/damping/parametric/status", V, Kind::text, true);
    b.add("/vras/{}/reverb/diffuser/count", V, Kind::integer);
    b.add("/vras/{}/reverb/diffusion/count", V, Kind::integer);
    b.add("/vras/{}/reverb/diffusion/enable", V, Kind::boolean);
    b.add("/vras/{}/reverb/{}/shapefactor", cat(V, 2), Kind::ratio);
    b.add("/vras/{}/reverb/{}/deltamax", cat(V, 2), Kind::ratio);
    b.add("/vras/{}/reverb/{}/diffuser/shapefactor", cat(V, 2), Kind::ratio);
    b.add("/vras/{}/reverb/{}/diffuser/delay/divisor", cat(V, 2), Kind::ratio);
    b.add("/vras/{}/reverb/{}/diffuser/level", cat(V, 2), Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add("/vras/{}/reverb/{}/damping/frequency", cat(V, 2), Kind::hertz, false, 10.0, 20000.0);
    b.add("/vras/{}/reverb/{}/damping/level", cat(V, 2), Kind::decibels, false, -18.0, 18.0);
    b.add("/vras/{}/reverb/{}/damping/parametric/bandbypass", cat(V, 5), Kind::boolean);
    b.add("/vras/{}/reverb/{}/damping/parametric/frequency", cat(V, 5), Kind::hertz, false, 10.0, 20000.0);
    b.add("/vras/{}/reverb/{}/damping/parametric/level", cat(V, 5), Kind::decibels, false, -18.0, 18.0);
    b.add("/vras/{}/reverb/{}/damping/parametric/q", cat(V, 5), Kind::ratio, false, 0.5, 10.0);
    b.add("/vras/{}/reverb/{}/damping/parametric/type", cat(V, 5), Kind::integer, false, 3.0, 10.0);
    b.add("/vras/{}/reverb/{}/damping/parametric/beta", cat(V, 5), Kind::ratio);
    b.add("/vras/{}/earlyreflection/{}/level", cat(V, shape.reflections), Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add("/vras/{}/earlyreflection/{}/mute", cat(V, shape.reflections), Kind::boolean);
    b.add("/vras/{}/earlyreflection/{}/delay", cat(V, shape.reflections), Kind::nanoseconds, false, 0.0, DELAY_MAX_NS);
    b.add("/vras/{}/earlyreflection/attenuation", V, Kind::decibels, false, -18.0, 18.0);
    b.add("/vras/{}/earlyreflection/frequency", V, Kind::hertz, false, 10.0, 20000.0);
    b.add("/vras/{}/earlyreflection/squeeze", V, Kind::ratio);
    b.add("/vras/{}/earlyreflection/short/spread", V, Kind::ratio);
    b.add("/vras/{}/earlyreflection/long/shapefactor", V, Kind::ratio);
    b.add("/vras/{}/earlyreflection/maximum/delay", V, Kind::nanoseconds, false, 0.0, DELAY_MAX_NS);
    b.add("/vras/{}/earlyreflection/input/geometry", V, Kind::integer);
    b.add("/vras/{}/earlyreflection/output/geometry", V, Kind::integer);

    // Monitor strips: isolate, AGC assignment, and a compressor.
    std::vector<std::uint16_t> const M{shape.monitors};
    b.add("/monitor/{}/name", M, Kind::text);
    b.add("/monitor/{}/isolate", M, Kind::boolean);
    b.add("/monitor/{}/agc/unit", M, Kind::integer);
    b.add("/monitor/{}/dynamics/makeupgain", M, Kind::decibels, false, DB_FLOOR, 50.0);
    b.add("/monitor/{}/dynamics/compressor/attack", M, Kind::nanoseconds, false, DYN_TIME_MIN_NS, DYN_TIME_MAX_NS);
    b.add("/monitor/{}/dynamics/compressor/bypass", M, Kind::boolean);
    b.add("/monitor/{}/dynamics/compressor/hold", M, Kind::nanoseconds, false, 0.0, DYN_TIME_MAX_NS);
    b.add("/monitor/{}/dynamics/compressor/ratio", M, Kind::ratio, false, 1.0, 20.0);
    b.add("/monitor/{}/dynamics/compressor/release", M, Kind::nanoseconds, false, DYN_TIME_MIN_NS, DYN_TIME_MAX_NS);
    b.add("/monitor/{}/dynamics/compressor/threshold", M, Kind::decibels, false, DB_FLOOR, DB_CEIL);

    // AGC units: gain control plus their own parametric EQ.
    std::vector<std::uint16_t> const A{shape.agcs};
    b.add("/agc/{}/name", A, Kind::text);
    b.add("/agc/{}/bypass", A, Kind::boolean);
    b.add("/agc/{}/isolate", A, Kind::boolean);
    b.add("/agc/{}/threshold", A, Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add("/agc/{}/up/rate", A, Kind::ratio);
    b.add("/agc/{}/down/rate", A, Kind::ratio);
    b.add("/agc/{}/maximum/gain", A, Kind::decibels, false, 0.0, 50.0);
    b.add("/agc/{}/eq/bypass", A, Kind::boolean);
    b.add("/agc/{}/parametric/bypass", A, Kind::boolean);
    b.add("/agc/{}/parametric/status", A, Kind::text, true);
    b.add("/agc/{}/parametric/{}/bandbypass", cat(A, shape.eq_bands), Kind::boolean);
    b.add("/agc/{}/parametric/{}/frequency", cat(A, shape.eq_bands), Kind::hertz, false, 10.0, 20000.0);
    b.add("/agc/{}/parametric/{}/level", cat(A, shape.eq_bands), Kind::decibels, false, -18.0, 18.0);
    b.add("/agc/{}/parametric/{}/q", cat(A, shape.eq_bands), Kind::ratio, false, 0.5, 10.0);
    b.add("/agc/{}/parametric/{}/type", cat(A, shape.eq_bands), Kind::integer, false, 3.0, 10.0);
    b.add("/agc/{}/parametric/{}/name", cat(A, shape.eq_bands), Kind::text);

    if (shape.master) {
        std::vector<std::uint16_t> const NONE{};
        for (char const* leaf :
             {"/master/system/select",
              "/master/input/select",
              "/master/input/isolate",
              "/master/output/select",
              "/master/output/solo",
              "/master/output/mute",
              "/master/output/isolate",
              "/master/output/invert",
              "/master/output/touched",
              "/master/output/eq/bypass",
              "/master/output/parametric/bypass",
              "/master/listen/select",
              "/master/listen/solo",
              "/master/listen/mute",
              "/master/listen/isolate",
              "/master/listen/invert",
              "/master/listen/touched",
              "/master/listen/eq/bypass",
              "/master/listen/parametric/bypass",
              "/master/vgroup/select",
              "/master/vgroup/solo",
              "/master/vgroup/mute",
              "/master/vgroup/isolate",
              "/master/vgroup/touched",
              "/master/monitor/select",
              "/master/monitor/isolate",
              "/master/agc/select",
              "/master/agc/isolate",
              "/master/agc/eq/bypass",
              "/master/agc/parametric/bypass",
              "/master/vras/select",
              "/master/row/select",
              "/master/column/select",
              "/master/script/select",
              "/master/cuelistplayer/select",
              "/master/wildtracks/select",
              "/master/wildtracks/channel/select",
              "/master/wildtracks/deck/select"}) {
            b.add(leaf, NONE, Kind::boolean);
        }
        for (char const* sub : {"input", "return", "output"}) {
            auto base = std::string("/master/vras/{}/") + sub;
            b.add(base + "/select", V, Kind::boolean);
            b.add(base + "/solo", V, Kind::boolean);
            b.add(base + "/mute", V, Kind::boolean);
            b.add(base + "/isolate", V, Kind::boolean);
            b.add(base + "/invert", V, Kind::boolean);
            b.add(base + "/eq/bypass", V, Kind::boolean);
            b.add(base + "/parametric/bypass", V, Kind::boolean);
        }
    }
    b.add("/input/{}/prefader/meter", {shape.inputs}, Kind::decibels, true, METER_FLOOR, 0.0);
    b.add("/input/{}/postfader/meter", {shape.inputs}, Kind::decibels, true, METER_FLOOR, 0.0);
    b.add("/bus/{}/prefader/meter", {shape.buses}, Kind::decibels, true, METER_FLOOR, 0.0);

    // Input x Bus crosspoints.
    b.add("/input/{}/bus/isolate", {shape.buses ? shape.inputs : std::uint16_t(0)}, Kind::boolean);
    b.add("/input/{}/bus/{}/level", {shape.inputs, shape.buses}, Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add("/input/{}/bus/{}/trim", {shape.inputs, shape.buses}, Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add("/input/{}/bus/{}/effective/level", {shape.inputs, shape.buses}, Kind::decibels, true, DB_FLOOR, DB_CEIL);

    // Bus x Output crosspoints.
    b.add("/bus/{}/output/isolate", {shape.outputs ? shape.buses : std::uint16_t(0)}, Kind::boolean);
    b.add("/bus/{}/output/{}/level", {shape.buses, shape.outputs}, Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add("/bus/{}/output/{}/trim", {shape.buses, shape.outputs}, Kind::decibels, false, DB_FLOOR, DB_CEIL);
    b.add("/bus/{}/output/{}/delay", {shape.buses, shape.outputs}, Kind::nanoseconds, false, 0.0, DELAY_MAX_NS);
    b.add("/bus/{}/output/{}/delay/bypass", {shape.buses, shape.outputs}, Kind::boolean);
    b.add("/bus/{}/output/{}/delay/trim", {shape.buses, shape.outputs}, Kind::nanoseconds, false, -DELAY_MAX_NS, DELAY_MAX_NS);
    b.add("/bus/{}/output/{}/delay/trim/bypass", {shape.buses, shape.outputs}, Kind::boolean);
    b.add("/bus/{}/output/{}/effective/level", {shape.buses, shape.outputs}, Kind::decibels, true, DB_FLOOR, DB_CEIL);
    b.add("/bus/{}/output/{}/effective/delay", {shape.buses, shape.outputs}, Kind::nanoseconds, true, 0.0, DELAY_MAX_NS);

    if (shape.system) {
        b.add("/system/level", {}, Kind::decibels, false, DB_FLOOR, DB_CEIL);
        b.add("/system/trim", {}, Kind::decibels, false, DB_FLOOR, DB_CEIL);
        b.add("/system/mute", {}, Kind::boolean);
        b.add("/system/hold", {}, Kind::boolean);
        b.add("/system/isolate", {}, Kind::boolean);
        b.add("/system/touched", {}, Kind::boolean, true);
        b.add("/system/meter/fault", {}, Kind::boolean, true);
        b.add("/system/status/info", {}, Kind::text, true);
        b.add("/system/status/level", {}, Kind::integer, true);
    }
    return b;
}

}  // namespace

std::vector<NadiaPoint> nadia_points(NadiaShape const& shape)
{
    return std::move(expand_catalogue(shape).points);
}

NadiaNamespace build_nadia_namespace(NadiaShape const& shape)
{
    auto expanded = expand_catalogue(shape);
    auto const& points = expanded.points;

    NadiaNamespace ns;
    ns.shape = shape;
    ns.patterns = std::move(expanded.patterns);

    controlplane::ModelBuilder model;
    controlplane::PathAdaptorBuilder paths(points.size());
    ns.kinds.reserve(points.size());
    ns.names.reserve(points.size());

    for (auto const& p : points) {
        controlplane::ControlPointDesc d;
        if (p.read_only) {
            d.flags = controlplane::PointFlags::read_only;
        }
        if (p.min < p.max) {
            d.min = p.min;
            d.max = p.max;
        }
        auto h = model.add(d);
        bool mapped = paths.map(p.path, h);
        assert(mapped);
        (void)mapped;
        ns.kinds.push_back(p.kind);
        ns.names.push_back(nadia_friendly_name(p.path));
        ns.name_index.emplace(nadia_name_key(p.path), h);
    }
    ns.model = model.build();
    ns.adaptor = paths.build();
    return ns;
}

std::optional<ControlPointId> NadiaNamespace::resolve_name(std::string_view friendly) const
{
    auto it = name_index.find(nadia_name_key(friendly));
    if (it == name_index.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<double> nadia_scalar_of(NadiaValueKind kind, double floor, std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    if (value.empty() || kind == NadiaValueKind::text) {
        return std::nullopt;
    }
    if (kind == NadiaValueKind::boolean) {
        if (value == "true" || value == "True" || value == "1") {
            return 1.0;
        }
        if (value == "false" || value == "False" || value == "0") {
            return 0.0;
        }
        return std::nullopt;
    }
    if (value == "-inf" || value == "-Inf" || value == "-INF") {
        return floor;
    }
    std::string text(value);
    char* end = nullptr;
    double num = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) {
        return std::nullopt;
    }
    // A trailing unit suffix ("dB", "ms") after the number is tolerated.
    return num;
}

std::string nadia_format_value(NadiaValueKind kind, double value)
{
    char buf[48];
    switch (kind) {
        case NadiaValueKind::boolean:
            return value != 0.0 ? "true" : "false";
        case NadiaValueKind::nanoseconds:
        case NadiaValueKind::integer:
            snprintf(buf, sizeof buf, "%lld", static_cast<long long>(value));
            return buf;
        case NadiaValueKind::decibels:
        case NadiaValueKind::hertz:
        case NadiaValueKind::ratio:
        case NadiaValueKind::text:
            break;
    }
    snprintf(buf, sizeof buf, "%.10g", value);
    return buf;
}

}  // namespace statusbar::controlplane_devices
