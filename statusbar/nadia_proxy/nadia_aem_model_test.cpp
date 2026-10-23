// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the generated nadia_proxy AEM model: descriptor counts
// for a small design, full binding coverage, value formats by kind,
// array chunking, blob round-trip, and atdecc_aem_validate.

#include "statusbar/nadia_proxy/nadia_aem_model.hpp"

#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/atdecc_tools/atdecc_aem_validate.hpp"
#include "statusbar/test/test.hpp"

#include <cstdio>
#include <cstring>

using namespace statusbar;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nadia_proxy;

namespace {

controlplane_devices::NadiaShape small_shape()
{
    controlplane_devices::NadiaShape s;
    s.inputs = 4;
    s.buses = 2;
    s.outputs = 2;
    s.listens = 1;
    s.vgroups = 1;
    s.eq_bands = 2;
    return s;
}

controlplane_devices::NadiaNamespace const& ns()
{
    static auto instance = controlplane_devices::build_nadia_namespace(small_shape());
    return instance;
}

NadiaAemModel const& model()
{
    static auto instance = build_nadia_proxy_aem_model(ns());
    return instance;
}

DescriptorControl control_at(NadiaAemModel const& m, std::uint16_t index)
{
    DescriptorControl c{};
    for (auto const& d : m.descriptors) {
        if (d.descriptor_type == DESCRIPTOR_CONTROL && d.descriptor_index == index) {
            std::memcpy(&c, d.bytes.data(), std::min(d.bytes.size(), sizeof c));
            break;
        }
    }
    return c;
}

}  // namespace

TEST(nadia_aem, descriptor_counts)
{
    auto const& m = model();
    EXPECT_EQ(m.count_of(DESCRIPTOR_ENTITY), 1u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_CONFIGURATION), 1u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_AUDIO_UNIT), 1u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_AVB_INTERFACE), 1u);
    EXPECT_EQ(m.count_of(DESCRIPTOR_LOCALE), 1u);
    // One block per strip (4 + 2 + 2 + 1 + 1) plus System.
    EXPECT_EQ(m.count_of(DESCRIPTOR_CONTROL_BLOCK), 11u);
    // Per DSP strip: 12 base + 7 delay/invert/meter + 2 eq + 2 bands x 4 +
    // 2 dynamics + 4 sections x 7 = 59 (+2 input meters, +1 bus meter);
    // input crosspoints: isolate + 3 arrays; bus crosspoints: isolate + 8
    // arrays; vgroup: 12; System: 9; Identify: 1.
    std::size_t const per_strip = 12 + 7 + 2 + 2 * 4 + 2 + 4 * 7;
    // Outputs additionally carry the 5-band U-Shaping EQ: bypass + 4 x
    // (frequency + slope) + 5 levels = 14 (a live Core's design vocabulary).
    std::size_t const ushaping = 1 + 4 * 2 + 5;
    std::size_t const expected =
        1 + 4 * (per_strip + 2 + 4) + 2 * (per_strip + 1 + 9) + 2 * (per_strip + ushaping) + 1 * per_strip + 12 + 9;
    EXPECT_EQ(m.count_of(DESCRIPTOR_CONTROL), expected);
    EXPECT_EQ(m.count_of(DESCRIPTOR_STRINGS), (expected + 11 + 5 + 6) / 7);
    EXPECT_TRUE(m.entity_model_id == nadia_proxy_entity_model_id(small_shape()));
    EXPECT_TRUE(m.entity_model_id != nadia_proxy_entity_model_id(controlplane_devices::NadiaShape{}));
    EXPECT_EQ(m.entity_model_id & ~0x7FFFFFull, 0x70B3D5EDC3800000ull);
}

TEST(nadia_aem, every_point_is_bound)
{
    auto const& m = model();
    EXPECT_EQ(m.binding.bound_count(), ns().size());
    for (controlplane::ControlPointId h = 0; h < ns().size(); ++h) {
        auto const& ref = m.binding.address_of(h);
        EXPECT_EQ(ref.descriptor_type, std::uint16_t(DESCRIPTOR_CONTROL));
        EXPECT_TRUE(m.binding.resolve(ref) == std::optional{h});
    }
    // Identify is control 0 and unbound.
    EXPECT_TRUE(!m.binding.resolve(controlplane::AemRef{0, DESCRIPTOR_CONTROL, 0, 0}).has_value());

    // EQ band grouping: frequency/level/q share one CONTROL.
    auto f = *ns().resolve_path("/input/2/parametric/1/frequency");
    auto l = *ns().resolve_path("/input/2/parametric/1/level");
    auto q = *ns().resolve_path("/input/2/parametric/1/q");
    auto rf = m.binding.address_of(f);
    EXPECT_EQ(m.binding.address_of(l).descriptor_index, rf.descriptor_index);
    EXPECT_EQ(m.binding.address_of(q).descriptor_index, rf.descriptor_index);
    EXPECT_EQ(rf.value_index, 0u);
    EXPECT_EQ(m.binding.address_of(l).value_index, 1u);
    EXPECT_EQ(m.binding.address_of(q).value_index, 2u);

    // Crosspoint rows are arrays indexed by destination.
    auto xp1 = m.binding.address_of(*ns().resolve_path("/bus/2/output/1/delay"));
    auto xp2 = m.binding.address_of(*ns().resolve_path("/bus/2/output/2/delay"));
    EXPECT_EQ(xp1.descriptor_index, xp2.descriptor_index);
    EXPECT_EQ(xp1.value_index, 0u);
    EXPECT_EQ(xp2.value_index, 1u);
    auto c = control_at(m, xp1.descriptor_index);
    EXPECT_TRUE(c.object_name.as_string_view() == "Bus 2 Output Delays");
    EXPECT_EQ(unpack_control_value_type(c.control_value_type.get()).value_type, std::uint16_t(CONTROL_ARRAY_INT64));
    EXPECT_EQ(c.number_of_values.get(), 2u);
}

TEST(nadia_aem, value_formats_by_kind)
{
    auto const& m = model();
    auto const& ns_ = ns();
    auto type_of = [&](char const* path) {
        auto ref = m.binding.address_of(*ns_.resolve_path(path));
        return unpack_control_value_type(control_at(m, ref.descriptor_index).control_value_type.get());
    };
    EXPECT_EQ(type_of("/input/1/level").value_type, std::uint16_t(CONTROL_LINEAR_FLOAT));
    EXPECT_EQ(type_of("/input/1/mute").value_type, std::uint16_t(CONTROL_LINEAR_UINT8));
    EXPECT_EQ(type_of("/input/1/delay").value_type, std::uint16_t(CONTROL_LINEAR_INT64));
    EXPECT_EQ(type_of("/input/1/parametric/1/type").value_type, std::uint16_t(CONTROL_LINEAR_INT32));
    EXPECT_EQ(type_of("/input/1/name").value_type, std::uint16_t(CONTROL_UTF8));
    EXPECT_TRUE(!type_of("/input/1/name").read_only);
    EXPECT_TRUE(type_of("/input/1/address").read_only);
    EXPECT_TRUE(type_of("/input/1/meter").read_only);
    EXPECT_TRUE(type_of("/system/status/level").read_only);

    // Input 1 Level is CONTROL[1]: the schema's dB range, 0.1 step, dB unit.
    for (auto const& d : m.descriptors) {
        if (d.descriptor_type != DESCRIPTOR_CONTROL || d.descriptor_index != 1) {
            continue;
        }
        LinearValueEntry<float> entry;
        std::memcpy(&entry, d.bytes.data() + DescriptorControl::LENGTH, sizeof entry);
        EXPECT_EQ(entry.minimum(), -90.f);
        EXPECT_EQ(entry.maximum(), 10.f);
        EXPECT_EQ(entry.step(), 0.1f);
        EXPECT_EQ(entry.default_value(), 0.f);
        EXPECT_EQ(std::uint8_t(entry.unit.code), 0xB0u);  // dB
    }
    // Delay: int64 ns, unit 10^-9 s.
    auto dref = m.binding.address_of(*ns_.resolve_path("/input/1/delay"));
    for (auto const& d : m.descriptors) {
        if (d.descriptor_type != DESCRIPTOR_CONTROL || d.descriptor_index != dref.descriptor_index) {
            continue;
        }
        LinearValueEntry<std::int64_t> entry;
        std::memcpy(&entry, d.bytes.data() + DescriptorControl::LENGTH, sizeof entry);
        EXPECT_EQ(entry.minimum(), 0);
        EXPECT_EQ(entry.maximum(), std::int64_t(500000000));
        EXPECT_EQ(std::int8_t(std::uint8_t(entry.unit.multiplier)), -9);
        EXPECT_EQ(std::uint8_t(entry.unit.code), 0x08u);  // seconds
    }
}

TEST(nadia_aem, long_rows_chunk)
{
    controlplane_devices::NadiaShape s;
    s.inputs = 1;
    s.buses = 70;
    s.eq_bands = 0;
    auto wide_ns = controlplane_devices::build_nadia_namespace(s);
    auto m = build_nadia_proxy_aem_model(wide_ns);
    EXPECT_EQ(m.binding.bound_count(), wide_ns.size());
    auto a = m.binding.address_of(*wide_ns.resolve_path("/input/1/bus/32/level"));
    auto b = m.binding.address_of(*wide_ns.resolve_path("/input/1/bus/33/level"));
    auto c = m.binding.address_of(*wide_ns.resolve_path("/input/1/bus/70/level"));
    EXPECT_EQ(a.value_index, 31u);
    EXPECT_EQ(b.descriptor_index, std::uint16_t(a.descriptor_index + 1));
    EXPECT_EQ(b.value_index, 0u);
    EXPECT_EQ(c.descriptor_index, std::uint16_t(a.descriptor_index + 2));
    EXPECT_EQ(c.value_index, 5u);
    EXPECT_TRUE(control_at(m, a.descriptor_index).object_name.as_string_view() == "Input 1 Bus Levels 1-32");
    EXPECT_TRUE(control_at(m, c.descriptor_index).object_name.as_string_view() == "Input 1 Bus Levels 65-70");
    EXPECT_EQ(control_at(m, c.descriptor_index).number_of_values.get(), 6u);
}

TEST(nadia_aem, localized_strings_resolve)
{
    auto const& m = model();
    auto storage = DescriptorStorage::create(m.blob);
    EXPECT_TRUE(storage.has_value());
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
}

TEST(nadia_aem, blob_round_trips)
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
}

TEST(nadia_aem, validates_cleanly)
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

// A venue-sized design: every point still binds, every descriptor index
// stays within 16 bits, the STRINGS/LOCALE bookkeeping holds up, and the
// model validates. Also reports the size so capacity regressions show up
// in the test log.
TEST(nadia_aem, full_design_scales)
{
    controlplane_devices::NadiaShape shape;
    shape.inputs = 96;
    shape.buses = 48;
    shape.outputs = 64;
    shape.listens = 8;
    shape.vgroups = 16;
    shape.eq_bands = 8;
    auto ns = controlplane_devices::build_nadia_namespace(shape);
    auto m = build_nadia_proxy_aem_model(ns);
    EXPECT_EQ(m.binding.bound_count(), ns.size());
    auto controls = m.count_of(DESCRIPTOR_CONTROL);
    EXPECT_TRUE(controls < 65535u);
    std::size_t blob_size = m.blob.size();
    printf(
        "full design: %zu control points, %zu CONTROL descriptors, %zu descriptors, blob %zu bytes\n",
        ns.size(),
        controls,
        m.descriptors.size(),
        blob_size);
    std::vector<atdecc_tools::RawDescriptor> raw;
    raw.reserve(m.descriptors.size());
    for (auto const& d : m.descriptors) {
        raw.push_back({d.descriptor_type, d.descriptor_index, d.bytes});
    }
    std::size_t errors = 0;
    for (auto const& f : atdecc_tools::validate_aem_model(raw)) {
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

TEST_MAIN(statusbar_nadia_proxy, nadia_aem_model_test)
