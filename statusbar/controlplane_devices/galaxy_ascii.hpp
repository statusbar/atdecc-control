// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The GALAXY ASCII control protocol (TCP port 25003), as documented in the
// programming guide: newline-terminated text lines. Command forms:
//
//   /path              get value            -> /path='value'
//   /path='value'      set value            (echoed as a value report)
//   ?/path             get description      -> ?{'/path': { ... }}
//   +/path <ms>        subscribe (throttle) -> value reports as they change
//   -/path             unsubscribe
//
// Paths may carry server-side regular expressions (/processing/input/\d/mute).
// A path may also be given in the device's alias form `@N` (the item index
// from the describe metadata); the device always replies with the
// canonical path, never the alias.
// This header is the pure codec: line framing, parsing, and command
// formatting. No sockets — see galaxy_client.hpp for transport.

namespace statusbar::controlplane_devices {

inline constexpr std::uint16_t GALAXY_ASCII_PORT = 25003;

// ---- command formatting -------------------------------------------------

std::string galaxy_get(std::string_view path_or_pattern);
std::string galaxy_set(std::string_view path, std::string_view value);
std::string galaxy_describe(std::string_view path_or_pattern);
std::string galaxy_subscribe(std::string_view path_or_pattern, unsigned throttle_ms);
std::string galaxy_unsubscribe(std::string_view path_or_pattern);

// ---- response parsing ---------------------------------------------------

enum class GalaxyLineKind : std::uint8_t
{
    empty,     ///< blank line
    value,     ///< /path='value' (path and value filled in)
    describe,  ///< ?{...} describe response
    other,     ///< anything else (errors, banners)
};

struct GalaxyParsedLine
{
    GalaxyLineKind kind = GalaxyLineKind::empty;
    std::string_view path;   ///< value lines only
    std::string_view value;  ///< value lines only, outer quotes stripped
};

/// Parses one complete line (no terminator). Views point into `line`.
GalaxyParsedLine parse_galaxy_line(std::string_view line);

/// Reassembles complete lines from arbitrary byte chunks. Terminators are
/// CR, LF, or CRLF (the guide allows either).
class GalaxyLineBuffer
{
  public:
    /// Feeds bytes; calls visit(std::string_view line) per complete line
    /// (terminator excluded, empty lines skipped).
    template <typename Visit>
    void feed(std::string_view bytes, Visit&& visit)
    {
        for (char c : bytes) {
            if (c == '\n' || c == '\r') {
                if (!pending_.empty()) {
                    visit(std::string_view(pending_));
                    pending_.clear();
                }
                continue;
            }
            pending_.push_back(c);
        }
    }

    /// Bytes of an unterminated trailing line still buffered.
    std::size_t pending() const noexcept { return pending_.size(); }

  private:
    std::string pending_;
};

}  // namespace statusbar::controlplane_devices
