// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane_devices/galaxy_mirror.hpp"

#include "statusbar/controlplane_devices/galaxy_ascii.hpp"

#include <algorithm>
#include <cstdlib>

namespace statusbar::controlplane_devices {

using controlplane::ControlPointId;

namespace {

/// Scalar encoding for the Instance's flat value array: booleans become
/// 0/1, plain decimal numbers their bit pattern; anything else (strings,
/// hex ids, dates) lives only in the mirror's string store.
std::optional<std::uint64_t> scalar_bits(std::string const& value)
{
    if (value == "true") {
        return controlplane::value_bits(1.0);
    }
    if (value == "false") {
        return controlplane::value_bits(0.0);
    }
    if (value.empty()) {
        return std::nullopt;
    }
    for (char c : value) {
        if ((c < '0' || c > '9') && c != '-' && c != '+' && c != '.' && c != 'e' && c != 'E') {
            return std::nullopt;
        }
    }
    char* end = nullptr;
    double num = std::strtod(value.c_str(), &end);
    if (end != value.c_str() + value.size()) {
        return std::nullopt;
    }
    return controlplane::value_bits(num);
}

}  // namespace

GalaxyMirror::GalaxyMirror()
    : GalaxyMirror(build_galaxy_816_namespace())
{}

GalaxyMirror::GalaxyMirror(GalaxyNamespace ns)
    : ns_(std::move(ns))
    , instance_(ns_.model, controlplane::Role::controller)
    , values_(ns_.size())
    , reported_(ns_.size())
{}

std::optional<ControlPointId> GalaxyMirror::consume_line(std::string_view line, std::uint64_t now_ns)
{
    auto parsed = parse_galaxy_line(line);
    switch (parsed.kind) {
        case GalaxyLineKind::empty:
            return std::nullopt;
        case GalaxyLineKind::describe:
            ++describe_count_;
            return std::nullopt;
        case GalaxyLineKind::other:
            ++other_count_;
            return std::nullopt;
        case GalaxyLineKind::value:
            break;
    }
    ++value_report_count_;
    auto h = ns_.resolve_address(parsed.path);
    if (!h) {
        std::string path(parsed.path);
        if (std::find(unknown_paths_.begin(), unknown_paths_.end(), path) == unknown_paths_.end()) {
            unknown_paths_.push_back(std::move(path));
        }
        return std::nullopt;
    }
    values_[*h].assign(parsed.value);
    reported_.set(*h);
    if (auto bits = scalar_bits(values_[*h])) {
        instance_.set_reported(*h, *bits, now_ns);
    }
    return h;
}

void GalaxyMirror::record_local_set(ControlPointId h, std::string_view value, std::uint64_t now_ns)
{
    values_[h].assign(value);
    reported_.set(h);
    if (auto bits = scalar_bits(values_[h])) {
        instance_.set_reported(h, *bits, now_ns);
    }
}

controlplane::Bitset GalaxyMirror::missing() const
{
    // missing = all & ~reported
    controlplane::Bitset out(ns_.size());
    out.set_all();
    reported_.for_each_set([&](ControlPointId h) { out.reset(h); });
    return out;
}

}  // namespace statusbar::controlplane_devices
