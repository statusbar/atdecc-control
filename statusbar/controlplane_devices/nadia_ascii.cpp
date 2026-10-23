// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane_devices/nadia_ascii.hpp"

#include <cctype>

namespace statusbar::controlplane_devices {

namespace {

/// Catalogue token spellings that plain capitalisation gets wrong.
struct TokenSpelling
{
    char const* lower;
    char const* display;
};

constexpr TokenSpelling TOKEN_SPELLINGS[] = {
    {"agc", "AGC"},
    {"avb", "AVB"},
    {"bandbypass", "BandBypass"},
    {"cal", "CAL"},
    {"cuelist", "CueList"},
    {"cuelistplayer", "CueListPlayer"},
    {"deltamax", "DeltaMax"},
    {"earlyreflection", "EarlyReflection"},
    {"eq", "EQ"},
    {"fadetime", "FadeTime"},
    {"id", "ID"},
    {"makeupgain", "MakeupGain"},
    {"mapfrom", "MapFrom"},
    {"noisegate", "NoiseGate"},
    {"osc", "OSC"},
    {"pagegroup", "PageGroup"},
    {"polarposition", "PolarPosition"},
    {"positionpercent", "PositionPercent"},
    {"postfader", "PostFader"},
    {"prefader", "PreFader"},
    {"rme", "RME"},
    {"rtp", "RTP"},
    {"shapefactor", "ShapeFactor"},
    {"spacemap", "SpaceMap"},
    {"timebase", "TimeBase"},
    {"timecode", "TimeCode"},
    {"ushaping", "UShaping"},
    {"vgroup", "VGroup"},
    {"vras", "VRAS"},
    {"waittime", "WaitTime"},
    {"wildtracks", "WildTracks"},
};

std::string display_token(std::string_view lower)
{
    for (auto const& t : TOKEN_SPELLINGS) {
        if (lower == t.lower) {
            return t.display;
        }
    }
    std::string out(lower);
    if (!out.empty()) {
        out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

template <typename Visit>
void each_segment(std::string_view path, Visit&& visit)
{
    std::size_t at = 0;
    while (at < path.size()) {
        if (path[at] == '/') {
            ++at;
            continue;
        }
        auto end = path.find('/', at);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        visit(path.substr(at, end - at));
        at = end;
    }
}

template <typename Visit>
void each_word(std::string_view text, Visit&& visit)
{
    std::size_t at = 0;
    while (at < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[at]))) {
            ++at;
            continue;
        }
        auto end = at;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        visit(text.substr(at, end - at));
        at = end;
    }
}

}  // namespace

std::string nadia_command_address(std::string_view path)
{
    std::string out;
    each_segment(path, [&](std::string_view seg) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(seg);
    });
    return out;
}

std::string nadia_friendly_name(std::string_view path)
{
    std::string out;
    each_segment(path, [&](std::string_view seg) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        bool numeric = !seg.empty();
        for (char c : seg) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                numeric = false;
                break;
            }
        }
        out.append(numeric ? std::string(seg) : display_token(seg));
    });
    return out;
}

std::string nadia_path_of_name(std::string_view friendly)
{
    std::string out;
    each_word(friendly, [&](std::string_view word) {
        out.push_back('/');
        for (char c : word) {
            out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        }
    });
    return out;
}

std::string nadia_name_key(std::string_view friendly_or_path)
{
    std::string out;
    out.reserve(friendly_or_path.size());
    for (char c : friendly_or_path) {
        if (c == '/' || std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string nadia_get(std::string_view command_addresses, std::string_view tag)
{
    std::string out = "get ";
    if (!tag.empty()) {
        out.append(tag);
        out += " = ";
    }
    out.append(command_addresses);
    out.push_back('\n');
    return out;
}

std::string nadia_set(std::string_view command_addresses, std::string_view value)
{
    std::string out = "set ";
    out.append(command_addresses);
    out += " = ";
    out.append(value);
    out.push_back('\n');
    return out;
}

std::string nadia_subscribe(std::string_view command_addresses)
{
    return "subscribe " + std::string(command_addresses) + "\n";
}

std::string nadia_unsubscribe(std::string_view command_addresses)
{
    return "unsubscribe " + std::string(command_addresses) + "\n";
}

std::string nadia_unsubscribe_all()
{
    return "unsubscribeall\n";
}

NadiaParsedLine parse_nadia_line(std::string_view line)
{
    NadiaParsedLine out;
    if (line.empty()) {
        return out;
    }
    constexpr std::string_view GOT = "Got";
    if (line.substr(0, GOT.size()) != GOT) {
        out.kind = NadiaLineKind::other;
        return out;
    }
    auto rest = line.substr(GOT.size());
    if (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    if (!rest.empty() && rest.front() == '(') {
        auto close = rest.find(')');
        if (close == std::string_view::npos) {
            out.kind = NadiaLineKind::other;
            return out;
        }
        out.tag = rest.substr(1, close - 1);
        rest.remove_prefix(close + 1);
    }
    if (rest.empty() || rest.front() != ':') {
        out.kind = NadiaLineKind::other;
        return out;
    }
    rest.remove_prefix(1);
    if (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    // The name never contains '=', so the first " = " is the split; the
    // value keeps everything after it verbatim (it may itself hold '=').
    auto eq = rest.find(" = ");
    std::string_view name;
    std::string_view value;
    if (eq == std::string_view::npos) {
        // A trailing "Name =" with nothing after the space.
        if (rest.size() >= 2 && rest.substr(rest.size() - 2) == " =") {
            name = rest.substr(0, rest.size() - 2);
        } else {
            out.kind = NadiaLineKind::other;
            return out;
        }
    } else {
        name = rest.substr(0, eq);
        value = rest.substr(eq + 3);
    }
    while (!name.empty() && name.back() == ' ') {
        name.remove_suffix(1);
    }
    out.kind = NadiaLineKind::value;
    out.name = name;
    out.value = value;
    out.unknown_address = name.empty() && value == "unset";
    return out;
}

}  // namespace statusbar::controlplane_devices
