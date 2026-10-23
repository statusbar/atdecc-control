// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane_devices/nadia_discover.hpp"

#include "statusbar/controlplane_devices/nadia_ascii.hpp"

#include <cstdlib>
#include <string>

namespace statusbar::controlplane_devices {

bool parse_nadia_shape(std::string_view text, NadiaShape& shape)
{
    std::size_t at = 0;
    while (at < text.size()) {
        auto end = text.find(',', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        auto item = text.substr(at, end - at);
        at = end + 1;
        auto eq = item.find('=');
        if (eq == std::string_view::npos) {
            return false;
        }
        auto key = item.substr(0, eq);
        auto value = std::uint16_t(atoi(std::string(item.substr(eq + 1)).c_str()));
        if (key == "inputs") {
            shape.inputs = value;
        } else if (key == "buses") {
            shape.buses = value;
        } else if (key == "outputs") {
            shape.outputs = value;
        } else if (key == "listens") {
            shape.listens = value;
        } else if (key == "vgroups") {
            shape.vgroups = value;
        } else if (key == "bands" || key == "eq_bands") {
            shape.eq_bands = value;
        } else if (key == "system") {
            shape.system = value != 0;
        } else if (key == "vras") {
            shape.vras = value;
        } else if (key == "vras_inputs") {
            shape.vras_inputs = value;
        } else if (key == "vras_returns") {
            shape.vras_returns = value;
        } else if (key == "vras_outputs") {
            shape.vras_outputs = value;
        } else if (key == "reflections") {
            shape.reflections = value;
        } else if (key == "monitors") {
            shape.monitors = value;
        } else if (key == "agcs") {
            shape.agcs = value;
        } else if (key == "master") {
            shape.master = value != 0;
        } else {
            return false;
        }
    }
    return true;
}

namespace {

/// Counts how many of "<family> 1-<max> <leaf>" answer with a value.
/// Returns -1 when the send fails.
int probe_count(GalaxyClient& client, std::string const& family, std::string const& leaf, unsigned max)
{
    auto command = family + " 1-" + std::to_string(max) + " " + leaf;
    if (!client.send(nadia_get(command))) {
        return -1;
    }
    int count = 0;
    client.drain(500, 30000, [&](std::string_view line) {
        auto parsed = parse_nadia_line(line);
        if (parsed.kind == NadiaLineKind::value && !parsed.unknown_address && !parsed.value.empty()) {
            ++count;
        }
    });
    return count;
}

/// True when a single unindexed address answers with a value.
bool probe_single(GalaxyClient& client, std::string const& address)
{
    if (!client.send(nadia_get(address))) {
        return false;
    }
    bool populated = false;
    client.drain(500, 30000, [&](std::string_view line) {
        auto parsed = parse_nadia_line(line);
        if (parsed.kind == NadiaLineKind::value && !parsed.unknown_address && !parsed.value.empty()) {
            populated = true;
        }
    });
    return populated;
}

}  // namespace

NadiaShape discover_nadia_shape(GalaxyClient& client, unsigned max, FILE* log)
{
    NadiaShape shape;
    // A strip exists when a value leaf answers non-empty. Which leaf the
    // design populates varies: the RZ's loaded design (2026-09) answers
    // `level` on every real strip but leaves `address` empty, while
    // `address` was the populated one before — so try `level` first and
    // fall back to `address`. Never count by `name`: the device
    // synthesizes default names ("Input-65") past any real boundary.
    auto count = [&](char const* family, unsigned ceiling) {
        auto n = probe_count(client, family, "level", ceiling);
        if (n <= 0) {
            n = probe_count(client, family, "address", ceiling);
        }
        return std::uint16_t(n < 0 ? 0 : n);
    };
    shape.inputs = count("input", max);
    shape.buses = count("bus", max);
    shape.outputs = count("output", max);
    shape.listens = count("listen", max);
    shape.vgroups = count("vgroup", max);
    // EQ bands live on every strip family; probe the first family the
    // design actually populates (an output-only design has no input 1).
    char const* banded = shape.inputs != 0 ? "input 1 parametric"
        : shape.buses != 0                 ? "bus 1 parametric"
        : shape.outputs != 0               ? "output 1 parametric"
        : shape.listens != 0               ? "listen 1 parametric"
                                           : nullptr;
    if (banded != nullptr) {
        auto n = probe_count(client, banded, "frequency", 32);
        shape.eq_bands = std::uint16_t(n < 0 ? 0 : n);
    }
    // Design-level processing families (vocabulary of a live Core's
    // design, see nadia_schema.hpp). VRAS
    // per-engine outputs are one send strip per system output — 128 per
    // engine on the RZ, over 75k points across 12 engines — so they are
    // NOT auto-discovered; opt in with --shape vras_outputs=N.
    {
        auto n = probe_count(client, "vras", "reverb duration", 16);
        shape.vras = std::uint16_t(n < 0 ? 0 : n);
    }
    if (shape.vras > 0) {
        auto n = probe_count(client, "vras 1 input", "level", 32);
        shape.vras_inputs = std::uint16_t(n < 0 ? 0 : n);
        n = probe_count(client, "vras 1 return", "level", 32);
        shape.vras_returns = std::uint16_t(n < 0 ? 0 : n);
        n = probe_count(client, "vras 1 earlyreflection", "level", 32);
        shape.reflections = std::uint16_t(n < 0 ? 0 : n);
    }
    {
        auto n = probe_count(client, "monitor", "isolate", 32);
        shape.monitors = std::uint16_t(n < 0 ? 0 : n);
        n = probe_count(client, "agc", "threshold", 32);
        shape.agcs = std::uint16_t(n < 0 ? 0 : n);
        shape.master = probe_single(client, "master output mute");
    }
    if (log) {
        fprintf(
            log,
            "discovered shape: inputs=%u,buses=%u,outputs=%u,listens=%u,vgroups=%u,bands=%u,"
            "vras=%u,vras_inputs=%u,vras_returns=%u,vras_outputs=%u,reflections=%u,monitors=%u,agcs=%u,master=%u\n",
            shape.inputs,
            shape.buses,
            shape.outputs,
            shape.listens,
            shape.vgroups,
            shape.eq_bands,
            shape.vras,
            shape.vras_inputs,
            shape.vras_returns,
            shape.vras_outputs,
            shape.reflections,
            shape.monitors,
            shape.agcs,
            unsigned(shape.master));
    }
    return shape;
}

}  // namespace statusbar::controlplane_devices
