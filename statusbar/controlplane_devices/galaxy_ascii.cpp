// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane_devices/galaxy_ascii.hpp"

namespace statusbar::controlplane_devices {

std::string galaxy_get(std::string_view path_or_pattern)
{
    return std::string(path_or_pattern) + "\n";
}

std::string galaxy_set(std::string_view path, std::string_view value)
{
    std::string out(path);
    out += "='";
    out += value;
    out += "'\n";
    return out;
}

std::string galaxy_describe(std::string_view path_or_pattern)
{
    return "?" + std::string(path_or_pattern) + "\n";
}

std::string galaxy_subscribe(std::string_view path_or_pattern, unsigned throttle_ms)
{
    return "+" + std::string(path_or_pattern) + " " + std::to_string(throttle_ms) + "\n";
}

std::string galaxy_unsubscribe(std::string_view path_or_pattern)
{
    return "-" + std::string(path_or_pattern) + "\n";
}

GalaxyParsedLine parse_galaxy_line(std::string_view line)
{
    GalaxyParsedLine out;
    if (line.empty()) {
        out.kind = GalaxyLineKind::empty;
        return out;
    }
    if (line.front() == '?') {
        out.kind = GalaxyLineKind::describe;
        return out;
    }
    if (line.front() != '/' && line.front() != '@') {
        out.kind = GalaxyLineKind::other;
        return out;
    }
    auto eq = line.find('=');
    if (eq == std::string_view::npos) {
        out.kind = GalaxyLineKind::other;
        return out;
    }
    out.kind = GalaxyLineKind::value;
    out.path = line.substr(0, eq);
    auto value = line.substr(eq + 1);
    // Values arrive single-quoted; strip the outer quotes but leave the
    // interior untouched (embedded double quotes are data).
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        value = value.substr(1, value.size() - 2);
    }
    out.value = value;
    return out;
}

}  // namespace statusbar::controlplane_devices
