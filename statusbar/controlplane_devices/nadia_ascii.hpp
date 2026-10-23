// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The Meyer LCS Core / Nadia CP ASCII text-command protocol (TCP ports
// 28034 and 28134; "core text commands" v1.47): newline-terminated text
// lines. The command forms the proxy uses:
//
//   get [tag =] <addresses>          -> Got (tag): <Name> = <value>   (one per point;
//                                       an untagged get is auto-tagged "req-N")
//   set <addresses> = <value>        (no reply; a subscription reports the change)
//   subscribe <addresses>            -> Got: <Name> = <value> now and on every change
//   unsubscribe <addresses> / unsubscribeall
//
// An unknown address answers "Got (tag):  = unset"; a known address with
// no loaded mixer design answers an empty value.
//
// Addresses. Every control point is a ControlPointAddress: semantic tokens
// (ciInput, ciLevel, ...) followed by zero-based indices. The text
// protocol speaks the "user-friendly" form — tokens minus the ci prefix,
// indices one-based and interlaced after successive tokens, so
// (ciBus, ciOutput, ciLevel, 5, 3) is "Bus 6 Output 4 Level". This codec
// keeps three spellings of one address:
//
//   canonical path   "/bus/6/output/4/level"   controlplane adaptor key
//   command form     "bus 6 output 4 level"    what get/set/subscribe take
//                                              (ranges allowed: "input 1-8 level")
//   friendly name    "Bus 6 Output 4 Level"    what Got: lines report
//
// Reply names are matched through nadia_name_key (case- and
// whitespace-insensitive), so a token the device happens to render with
// an interior space or different capitalisation still resolves.
// No sockets — GalaxyClient is the line-oriented transport for both
// Meyer protocols.

namespace statusbar::controlplane_devices {

inline constexpr std::uint16_t NADIA_ASCII_PORT = 28034;
inline constexpr std::uint16_t NADIA_ASCII_PORT_ALT = 28134;

// ---- address spellings --------------------------------------------------

/// "/input/1/level" -> "input 1 level".
std::string nadia_command_address(std::string_view path);

/// "/input/1/level" -> "Input 1 Level" (token display spellings from the
/// catalogue: "vgroup" -> "VGroup", "eq" -> "EQ", "noisegate" ->
/// "NoiseGate"; unknown tokens are capitalised).
std::string nadia_friendly_name(std::string_view path);

/// "Input 1 Level" -> "/input/1/level" (lower-cased, one segment per
/// whitespace-separated word).
std::string nadia_path_of_name(std::string_view friendly);

/// Matching key for a reported name: lower-cased with all whitespace
/// removed ("Input 1 Level" and "input1level" agree).
std::string nadia_name_key(std::string_view friendly_or_path);

// ---- command formatting -------------------------------------------------

std::string nadia_get(std::string_view command_addresses, std::string_view tag = {});
std::string nadia_set(std::string_view command_addresses, std::string_view value);
std::string nadia_subscribe(std::string_view command_addresses);
std::string nadia_unsubscribe(std::string_view command_addresses);
std::string nadia_unsubscribe_all();

// ---- response parsing ---------------------------------------------------

enum class NadiaLineKind : std::uint8_t
{
    empty,  ///< blank line
    value,  ///< Got[ (tag)]: Name = value
    other,  ///< anything else (log lines, errors, events)
};

struct NadiaParsedLine
{
    NadiaLineKind kind = NadiaLineKind::empty;
    std::string_view tag;          ///< value lines: the get tag ("req-13", "foo"), empty for subscription pushes
    std::string_view name;         ///< value lines: the friendly name, empty when the address was unknown
    std::string_view value;        ///< value lines: verbatim, possibly empty
    bool unknown_address = false;  ///< value line with an empty name and value "unset"
};

/// Parses one complete line (no terminator). Views point into `line`.
NadiaParsedLine parse_nadia_line(std::string_view line);

}  // namespace statusbar::controlplane_devices
