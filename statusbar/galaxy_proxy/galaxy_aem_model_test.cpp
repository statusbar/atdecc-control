// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the generated galaxy_proxy AEM model: descriptor counts,
// binding coverage, blob round-trip, and validation with statusbar-avb's
// atdecc_aem_validate (the M5 acceptance gate).

#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"

#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/atdecc_tools/atdecc_aem_validate.hpp"
#include "statusbar/test/test.hpp"

#include <cstdio>
#include <cstring>

using namespace statusbar;
using namespace statusbar::atdecc::aem;
using namespace statusbar::galaxy_proxy;

namespace {

controlplane_devices::GalaxyNamespace const& ns()
{
    static auto instance = controlplane_devices::build_galaxy_816_namespace();
    return instance;
}

GalaxyAemModel const& model()
{
    static auto instance = build_galaxy_proxy_aem_model(ns());
    return instance;
}

}  // namespace

TEST(galaxy_aem, descriptor_counts)
{
    auto const& m = model();
    EXPECT_EQ(m.count_of(DESCRIPTOR_ENTITY), 1u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_CONFIGURATION), 1u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_AUDIO_UNIT), 1u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_MATRIX), 4u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_MATRIX_SIGNAL), 48u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_SIGNAL_SELECTOR), 8u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_CONTROL_BLOCK), 36u);  // 8 in + 16 out + 12 link groups
    // Every named descriptor's manufacturer name also lands in the
    // localized string table: 1021 controls + 8 selectors + 4 matrices +
    // 36 blocks + entity vendor/model + config + unit + interface = 1074
    // strings, 7 per STRINGS descriptor.
    EXPECT_EQ(m.count_of(DESCRIPTOR_LOCALE), 1u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_STRINGS), 154u);
    // Inside PLAN §5.2's descriptor budget; the exact count is pinned so
    // any generation-shape drift is a conscious act (and an
    // entity_model_id bump).
    EXPECT_EQ(m.count_of(DESCRIPTOR_CONTROL), 1021u);
    EXPECT_EQ(m.descriptors.size(), 1276u);
}

TEST(galaxy_aem, localized_strings_resolve)
{
    auto const& m = model();
    auto storage = DescriptorStorage::create(m.blob);
    EXPECT_TRUE(storage.has_value());

    // Every CONTROL's localized_description must resolve through the
    // string table to exactly its authored object_name — the fixed
    // manufacturer identity underneath the user-settable sticker.
    for (std::uint16_t i = 0; i < m.count_of(DESCRIPTOR_CONTROL); ++i) {
        auto const blob = storage->get_descriptor(0, DESCRIPTOR_CONTROL, i);
        EXPECT_TRUE(blob.has_value());
        DescriptorControl c{};
        span_load_padded(c, *blob);
        auto const ref = parse_localized_description(c.localized_description.get());
        EXPECT_TRUE(ref.has_value());
        if (!ref.has_value()) {
            break;
        }
        auto const str_blob = storage->get_descriptor(0, DESCRIPTOR_STRINGS, ref->strings_descriptor_index);
        EXPECT_TRUE(str_blob.has_value());
        DescriptorStrings str{};
        span_load_padded(str, *str_blob);
        EXPECT_TRUE(str.as_string_array()[ref->string_offset]->as_string_view() == c.object_name.as_string_view());
    }

    // The LOCALE covers the whole table from base 0.
    auto const loc_blob = storage->get_descriptor(0, DESCRIPTOR_LOCALE, 0);
    EXPECT_TRUE(loc_blob.has_value());
    DescriptorLocale locale{};
    span_load_padded(locale, *loc_blob);
    EXPECT_TRUE(locale.locale_identifier.as_string_view() == "en-US");
    EXPECT_EQ(locale.number_of_strings.get(), m.count_of(DESCRIPTOR_STRINGS));
    EXPECT_EQ(locale.base_strings.get(), 0u);
}

TEST(galaxy_aem, meter_banks_per_model)
{
    // Bank lookup: the three datasheet rows, GX-816 fallback for unknowns
    // (including the guide-listed Bluehorn 816).
    EXPECT_TRUE(std::string_view(galaxy_model_banks("GX-408").model_string) == "GX-408");
    EXPECT_TRUE(std::string_view(galaxy_model_banks("GX-816-AES3").model_string) == "GX-816-AES3");
    EXPECT_TRUE(std::string_view(galaxy_model_banks("GX-BLUEHORN").model_string) == "GX-816");
    EXPECT_TRUE(std::string_view(galaxy_model_banks("").model_string) == "GX-816");

    // Every bank row is a distinct served model and must carry its own
    // entity_model_id — controllers cache descriptor sets by that id.
    EXPECT_EQ(galaxy_model_banks("GX-816").entity_model_id, GALAXY_PROXY_ENTITY_MODEL_ID_GX816);
    EXPECT_EQ(galaxy_model_banks("GX-408").entity_model_id, GALAXY_PROXY_ENTITY_MODEL_ID_GX408);
    EXPECT_EQ(galaxy_model_banks("GX-816-AES3").entity_model_id, GALAXY_PROXY_ENTITY_MODEL_ID_GX816AES3);
    EXPECT_TRUE(GALAXY_PROXY_ENTITY_MODEL_ID_GX816 != GALAXY_PROXY_ENTITY_MODEL_ID_GX408);
    EXPECT_TRUE(GALAXY_PROXY_ENTITY_MODEL_ID_GX408 != GALAXY_PROXY_ENTITY_MODEL_ID_GX816AES3);

    // The DSP engine is family-wide (32x16 matrix, 16 processed outputs);
    // physical banks follow the connectors.
    auto const& b408 = galaxy_model_banks("GX-408");
    EXPECT_EQ(b408.input_meters, 4u);
    EXPECT_EQ(b408.matrix_input_meters, 32u);
    EXPECT_EQ(b408.output_meters, 16u);
    EXPECT_EQ(b408.sim_meters, 1u);
    EXPECT_EQ(b408.line_mon, 8u);

    // A 408 model builds against the shared namespace: same descriptor
    // shape, smaller meter banks.
    auto m408 = build_galaxy_proxy_aem_model(ns(), b408);
    EXPECT_EQ(m408.descriptors.size(), model().descriptors.size());
    std::size_t checked = 0;
    for (auto const& d : m408.descriptors) {
        if (d.descriptor_type != DESCRIPTOR_CONTROL) {
            continue;
        }
        DescriptorControl c;
        std::memcpy(&c, d.bytes.data(), std::min(d.bytes.size(), sizeof c));
        auto name = std::string(c.object_name.as_string_view());
        auto values = std::uint16_t(c.number_of_values.get());
        if (name == "Input Meters" || name == "Input Split Meters") {
            EXPECT_EQ(values, 4u);
            ++checked;
        } else if (name == "SIM Meters") {
            EXPECT_EQ(values, 1u);
            ++checked;
        } else if (name.starts_with("Line Monitor")) {
            EXPECT_EQ(values, 8u);
            ++checked;
        } else if (name == "Matrix Input Meters") {
            EXPECT_EQ(values, 32u);
            ++checked;
        } else if (name == "Output Meters") {
            EXPECT_EQ(values, 16u);
            ++checked;
        }
    }
    EXPECT_EQ(checked, 8u);
}

TEST(galaxy_aem, bindings_cover_the_dsp_surface)
{
    auto const& m = model();
    // Every processing point has an AEM projection.
    auto check_bound = [&](char const* path) {
        auto h = ns().adaptor.resolve(path);
        EXPECT_TRUE(h.has_value());
        EXPECT_TRUE(m.binding.address_of(*h).descriptor_type != controlplane::AemRef::NO_DESCRIPTOR);
    };
    check_bound("/processing/input/1/gain");
    check_bound("/processing/input/8/eq/5/gain");
    check_bound("/processing/output/16/lowpass/frequency");
    check_bound("/processing/matrix/32/16/gain");
    check_bound("/device/input/1/mode");
    check_bound("/device/output/16/sim/trim");
    check_bound("/status/meter/matrix_input/32");
    check_bound("/project/snapshot/active/id");

    // EQ band grouping: frequency/bandwidth/gain share one CONTROL.
    auto f = *ns().adaptor.resolve("/processing/input/1/eq/2/frequency");
    auto bw = *ns().adaptor.resolve("/processing/input/1/eq/2/bandwidth");
    auto g = *ns().adaptor.resolve("/processing/input/1/eq/2/gain");
    auto rf = m.binding.address_of(f);
    EXPECT_EQ(rf.descriptor_type, std::uint16_t(DESCRIPTOR_CONTROL));
    EXPECT_EQ(m.binding.address_of(bw).descriptor_index, rf.descriptor_index);
    EXPECT_EQ(m.binding.address_of(g).descriptor_index, rf.descriptor_index);
    EXPECT_EQ(rf.value_index, 0u);
    EXPECT_EQ(m.binding.address_of(bw).value_index, 1u);
    EXPECT_EQ(m.binding.address_of(g).value_index, 2u);
    EXPECT_EQ(m.binding.values_of(DESCRIPTOR_CONTROL, rf.descriptor_index).count(), 3u);

    // Matrix crosspoints bind by grid position.
    auto xp = *ns().adaptor.resolve("/processing/matrix/3/5/gain");
    auto rxp = m.binding.address_of(xp);
    EXPECT_EQ(rxp.descriptor_type, std::uint16_t(DESCRIPTOR_MATRIX));
    EXPECT_EQ(rxp.value_index, std::uint16_t((3 - 1) * 16 + (5 - 1)));
    EXPECT_EQ(m.binding.values_of(DESCRIPTOR_MATRIX, rxp.descriptor_index).count(), 512u);

    // Snapshots/network/project metadata deliberately have no projection
    // (PLAN §5.4).
    auto snap = *ns().adaptor.resolve("/project/snapshot/7/name");
    EXPECT_EQ(m.binding.address_of(snap).descriptor_type, controlplane::AemRef::NO_DESCRIPTOR);
}

TEST(galaxy_aem, control_value_blocks_carry_live_ranges)
{
    // Input 1 Gain is CONTROL[1]; its linear value block must carry the
    // device's describe metadata, not the authored placeholder.
    auto const& m = model();
    for (auto const& d : m.descriptors) {
        if (d.descriptor_type != DESCRIPTOR_CONTROL || d.descriptor_index != 1) {
            continue;
        }
        LinearValueEntry<float> entry;
        std::memcpy(&entry, d.bytes.data() + DescriptorControl::LENGTH, sizeof entry);
        EXPECT_EQ(entry.minimum(), -90.f);
        EXPECT_EQ(entry.maximum(), 10.f);
        EXPECT_EQ(entry.step(), 0.1f);
        EXPECT_EQ(std::uint8_t(entry.unit.code), 0xB0u);  // dB
        return;
    }
    EXPECT_TRUE(false);  // CONTROL[1] not found
}

TEST(galaxy_aem, blob_round_trips)
{
    auto const& m = model();
    auto storage = DescriptorStorage::create(m.blob);
    EXPECT_TRUE(storage.has_value());
    for (auto const& d : m.descriptors) {
        auto got = storage->get_descriptor(d.configuration, d.descriptor_type, d.descriptor_index);
        EXPECT_TRUE(got.has_value());
        EXPECT_EQ(got->size(), d.bytes.size());
        EXPECT_TRUE(std::equal(got->begin(), got->end(), d.bytes.begin()));
    }
    // The CONFIGURATION descriptor carries its full descriptor_counts
    // trailer (74 + 4N), not the bare header (PLAN §6.2's warning).
    auto cfg = storage->get_descriptor(0, DESCRIPTOR_CONFIGURATION, 0);
    EXPECT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->size(), DescriptorConfiguration::LENGTH + 9 * 4);
}

TEST(galaxy_aem, validates_cleanly)
{
    auto const& m = model();
    std::vector<atdecc_tools::RawDescriptor> raw;
    raw.reserve(m.descriptors.size());
    for (auto const& d : m.descriptors) {
        raw.push_back({d.descriptor_type, d.descriptor_index, d.bytes});
    }
    auto findings = atdecc_tools::validate_aem_model(raw);
    std::size_t errors = 0;
    for (auto const& f : findings) {
        if (f.severity == atdecc_tools::Severity::Error) {
            ++errors;
            printf("VALIDATE ERROR %s: %s\n", f.where.c_str(), f.message.c_str());
        }
    }
    EXPECT_EQ(errors, 0u);
}

//
// Test Runner
//

TEST_MAIN(statusbar_galaxy_proxy, galaxy_aem_model_test)
