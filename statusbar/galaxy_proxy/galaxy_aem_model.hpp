// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane_aem_binding.hpp"
#include "statusbar/controlplane_devices/galaxy_schema.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// The galaxy_proxy companion entity's AEM model (docs/PLAN.md §5, §7):
// ENTITY + one CONFIGURATION + AUDIO_UNIT + CONTROL / CONTROL_BLOCK /
// MATRIX / MATRIX_SIGNAL / SIGNAL_SELECTOR, generated from the GALAXY 816
// namespace and emitted as raw wire descriptors plus the `.aem` blob. No
// TALKER/LISTENER capabilities, no streams — the proxy mirrors control
// only.
//
// Vendor control types are PROVISIONAL: allocated under the JDKS OUI-36
// (70:B3:D5:ED:C, bank 0x1) until the Meyer 00-1c-ab registry allocation
// is obtained (PLAN open decision #4). Bump ENTITY_MODEL_ID whenever the
// generation logic changes shape, or controllers will serve stale cached
// models (PLAN §7.1).
//
// Value ranges (min/max/step) are conservative placeholders pending the
// live-device describe capture from M4; refine before shipping.

namespace statusbar::atdecc::aem {
struct SignalSource;
}

namespace statusbar::galaxy_proxy {

/// Provisional entity_model_ids (JDKS OUI-36), one per GalaxyModelBanks
/// row: controllers cache descriptor sets by entity_model_id, so every
/// distinct served model — including the per-model meter-bank variants —
/// must carry its own id (IEEE 1722.1 Clause 6.2.1.9). Bump the whole
/// bank on any generation-shape change; 0x0003 and the 0x10 bank served
/// earlier content and are retired (0x10 -> 0x20: authored mode-selector
/// sources).
// 0x...20/21/22 were the pre-localized-strings generation of these
// three models; a shape change burns one id per variant (PLAN §7.1).
inline constexpr std::uint64_t GALAXY_PROXY_ENTITY_MODEL_ID_GX816 = 0x70B3D5EDC1000023ull;
inline constexpr std::uint64_t GALAXY_PROXY_ENTITY_MODEL_ID_GX408 = 0x70B3D5EDC1000024ull;
inline constexpr std::uint64_t GALAXY_PROXY_ENTITY_MODEL_ID_GX816AES3 = 0x70B3D5EDC1000025ull;

/// The GALAXY input-mode enumeration (0:NULL 1:ANALOG 2:AES3L 3:AES3R
/// 4:AVB 5:SIGNAL). The proxy models no signal path, so a mode selector's
/// sources are authored as {signal_type INVALID, signal_index = mode}:
/// self-describing, and mode 0 (NULL) is exactly AEM's NO_SIGNAL.
inline constexpr std::uint16_t GALAXY_INPUT_MODE_COUNT = 6;
inline constexpr std::uint16_t GALAXY_MODE_SIGNAL_TYPE = 0xFFFF;  // DESCRIPTOR_INVALID

/// The authored source triple for @p mode: {INVALID, mode, 0}.
atdecc::aem::SignalSource galaxy_input_mode_source(std::uint16_t mode);

/// The mode a selector source encodes, nullopt for a non-mode triple.
std::optional<std::uint16_t> galaxy_input_mode_of(atdecc::aem::SignalSource const& source);

struct GalaxyAemModel
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

    std::size_t count_of(std::uint16_t descriptor_type) const;
};

/// Per-model meter-bank widths, folded from the product datasheets. The
/// whole family shares one DSP engine — "The GALAXY 816 processor has the
/// same audio processing capability in a 2RU chassis with different
/// physical I/O" (GALAXY 408 datasheet) — so the DSP-side banks (matrix
/// inputs, processed outputs) are family-wide; the physical-side banks
/// follow each model's connectors. Only GX-816 is live-verified (the LA
/// Q1, fw 2.10.0); the other rows are datasheet-derived and provisional
/// until a device of that model is captured.
struct GalaxyModelBanks
{
    char const* model_string;           ///< /status/model_string, e.g. "GX-816"
    std::uint16_t input_meters;         ///< processed inputs (A..)
    std::uint16_t input_split_meters;   ///< input splitter taps (= processed inputs)
    std::uint16_t matrix_input_meters;  ///< 32 — sparse 32x16 matrix on every model
    std::uint16_t output_meters;        ///< 16 — processed outputs on every model
    std::uint16_t sim_meters;           ///< SIM 3 bus ports
    std::uint16_t line_mon;             ///< analog-capable output lines (ideal/neg/pos)

    /// This variant's entity_model_id — distinct per row, because the
    /// meter-bank widths change the served descriptor set and controllers
    /// cache models by this id.
    std::uint64_t entity_model_id;
};

/// The bank row for a /status/model_string; unknown models (including the
/// guide-listed Bluehorn 816, whose control surface is undocumented) fall
/// back to GX-816.
GalaxyModelBanks const& galaxy_model_banks(std::string_view model_string);

/// Generates the full model against the given GALAXY namespace, sizing the
/// meter banks for the given model (default: the live-verified GX-816).
GalaxyAemModel build_galaxy_proxy_aem_model(controlplane_devices::GalaxyNamespace const& ns);
GalaxyAemModel build_galaxy_proxy_aem_model(controlplane_devices::GalaxyNamespace const& ns, GalaxyModelBanks const& banks);

}  // namespace statusbar::galaxy_proxy
