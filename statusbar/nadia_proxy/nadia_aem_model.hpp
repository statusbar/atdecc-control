// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane_aem_binding.hpp"
#include "statusbar/controlplane_devices/nadia_schema.hpp"

#include <cstdint>
#include <vector>

// The nadia_proxy companion entity's AEM model (docs/PLAN.md §13): ENTITY +
// one CONFIGURATION + AUDIO_UNIT + CONTROL / CONTROL_BLOCK + LOCALE /
// STRINGS, generated from a NadiaNamespace — so from the loaded mixer
// design's shape — and emitted as raw wire descriptors plus the `.aem`
// blob. No TALKER/LISTENER capabilities, no streams: the proxy mirrors
// control only.
//
// Layout. Control 0 is IDENTIFY; then one CONTROL_BLOCK per channel strip
// (Input i, Bus i, Output i, Listen i, VGroup i) holding that strip's
// points in catalogue order, and a System block last. The Input×Bus and
// Bus×Output crosspoints are array controls inside the source strip's
// block ("Input 3 Bus Levels" = /input/3/bus/{1..B}/level), chunked at
// NADIA_ARRAY_CHUNK values, rather than MATRIX descriptors: a design's
// crosspoint parameters (levels, trims, delays, bypasses, effective
// values) would need eleven MATRIX grids, and the array form keeps every
// point addressable as (control, value_index) like the rest.
//
// Value formats follow NadiaValueKind: dB / Hz / ratio are LINEAR_FLOAT,
// booleans LINEAR_UINT8 0/255, nanosecond times LINEAR_INT64 with unit
// 10^-9 s (exact — no float rounding of the device's integer ns), EQ type
// and status enumerations LINEAR_INT32, names and addresses CONTROL_UTF8
// (writable where the device lets a controller rename). EQ bands are one
// grouped {frequency, level, q} control.
//
// Vendor control types are PROVISIONAL under the JDKS OUI-36 (70:B3:D5:ED:C,
// bank 0x3). The entity_model_id is derived from the shape — every distinct
// shape is a distinct served model and controllers cache descriptor sets
// by that id — with NADIA_PROXY_MODEL_GENERATION folded in; bump it
// whenever the generation logic changes shape.

namespace statusbar::nadia_proxy {

/// Folded into every derived entity_model_id; bump on any change to the
/// generated descriptor shape (PLAN §7.1 discipline).
inline constexpr std::uint32_t NADIA_PROXY_MODEL_GENERATION = 1;

/// Values per array-control chunk (crosspoint rows longer than this split
/// into "Name 1-32", "Name 33-64", ...).
inline constexpr std::uint16_t NADIA_ARRAY_CHUNK = 32;

/// JDKS OUI-36 bank 0x3 | a 23-bit hash of (generation, shape), bit 23
/// set so the ids never collide with the bank's vendor control types.
std::uint64_t nadia_proxy_entity_model_id(controlplane_devices::NadiaShape const& shape);

struct NadiaAemModel
{
    struct Raw
    {
        std::uint16_t configuration;
        std::uint16_t descriptor_type;
        std::uint16_t descriptor_index;
        std::vector<std::uint8_t> bytes;  ///< wire format, trailers included
    };

    std::vector<Raw> descriptors;      ///< TOC order: ENTITY first
    controlplane::AemBinding binding;  ///< handle <-> AEM tuple
    std::vector<std::uint8_t> blob;    ///< the .aem image
    std::uint64_t entity_model_id = 0;

    std::size_t count_of(std::uint16_t descriptor_type) const;
};

/// Generates the full model for the namespace's shape. Every namespace
/// point is bound to exactly one (CONTROL, value_index).
NadiaAemModel build_nadia_proxy_aem_model(controlplane_devices::NadiaNamespace const& ns);

}  // namespace statusbar::nadia_proxy
