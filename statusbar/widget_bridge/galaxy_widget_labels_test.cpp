// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// GalaxyWidgetLabels against the real generated model: the provider must
// answer the device's verbatim enum labels for an enumerated control and
// nothing for a continuous one — with the control located through the
// same binding the proxy serves, so a drifting model breaks this test.

#include "statusbar/widget_bridge/galaxy_widget_labels.hpp"

#include "statusbar/controlplane_devices/galaxy_schema.hpp"
#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"
#include "statusbar/test/test.hpp"

#include <string>

using namespace statusbar;
using namespace statusbar::widget_bridge;

namespace {

/// The CONTROL descriptor index bound to @p path in the GX-816 model.
auto control_index_of(std::string const& path) -> std::optional<std::uint16_t>
{
    auto const ns = controlplane_devices::build_galaxy_816_namespace();
    auto const model = galaxy_proxy::build_galaxy_proxy_aem_model(ns, galaxy_proxy::galaxy_model_banks("GX-816"));
    auto const handle = ns.resolve_address(path);
    if (!handle.has_value()) {
        return std::nullopt;
    }
    auto const& ref = model.binding.address_of(*handle);
    if (ref.descriptor_type != 0x001a) {
        return std::nullopt;
    }
    return ref.descriptor_index;
}

}  // namespace

TEST(galaxy_widget_labels, enumerated_and_continuous_controls)
{
    GalaxyWidgetLabels provider;

    // The output highpass type is a 12-label enum bound to a CONTROL
    // (/device/input/N/mode is enumerated too, but it projects as a
    // SIGNAL_SELECTOR, outside the widget layer's CONTROL scope).
    // Note the device's own quirks survive verbatim: values start at 1
    // and value 11 carries two labels.
    auto const highpass_index = control_index_of("/processing/output/1/highpass/type");
    EXPECT_TRUE(highpass_index.has_value());
    if (highpass_index.has_value()) {
        auto const options = provider.options_for(galaxy_proxy::GALAXY_PROXY_ENTITY_MODEL_ID_GX816, *highpass_index, false);
        EXPECT_EQ(options.size(), 12U);
        if (options.size() == 12) {
            EXPECT_TRUE(options[0].value == 1.0 && options[0].label == "BUTTERWORTH_6DB");
            EXPECT_TRUE(options[8].value == 9.0 && options[8].label == "LINKWITZ_RILEY_12DB");
            EXPECT_TRUE(options[10].value == 11.0 && options[11].value == 11.0);
        }
    }

    // A continuous control has no enumeration.
    auto const gain_index = control_index_of("/processing/input/1/gain");
    EXPECT_TRUE(gain_index.has_value());
    if (gain_index.has_value()) {
        EXPECT_TRUE(provider.options_for(galaxy_proxy::GALAXY_PROXY_ENTITY_MODEL_ID_GX816, *gain_index, false).empty());
    }

    // A non-galaxy model id costs one miss and answers nothing.
    EXPECT_TRUE(provider.options_for(0x1122334455667788ULL, 0, false).empty());

    // Matrix rows: the 8 processed inputs then the 24 matrix inputs,
    // shared by every crosspoint-parameter matrix; only 32-row matrices
    // of a galaxy model qualify.
    auto const rows = provider.matrix_row_labels(galaxy_proxy::GALAXY_PROXY_ENTITY_MODEL_ID_GX816, 0, 32);
    EXPECT_EQ(rows.size(), 32U);
    if (rows.size() == 32) {
        EXPECT_TRUE(rows[0] == "Input 1" && rows[7] == "Input 8");
        EXPECT_TRUE(rows[8] == "Matrix In 1" && rows[31] == "Matrix In 24");
    }
    EXPECT_TRUE(provider.matrix_row_labels(galaxy_proxy::GALAXY_PROXY_ENTITY_MODEL_ID_GX816, 0, 16).empty());
    EXPECT_TRUE(provider.matrix_row_labels(0x1122334455667788ULL, 0, 32).empty());

    // The mode SIGNAL_SELECTORs answer the INPUT_MODE_* enum through the
    // selector side of the interface: positions == mode values, by the
    // model's authored-source convention.
    auto const modes = provider.options_for(galaxy_proxy::GALAXY_PROXY_ENTITY_MODEL_ID_GX816, 0, true);
    EXPECT_EQ(modes.size(), 6U);
    if (modes.size() == 6) {
        EXPECT_TRUE(modes[0].value == 0.0 && modes[0].label == "INPUT_MODE_NULL");
        EXPECT_TRUE(modes[1].value == 1.0 && modes[1].label == "INPUT_MODE_ANALOG");
        EXPECT_TRUE(modes[4].value == 4.0 && modes[4].label == "INPUT_MODE_AVB");
    }
}

//
// Test Runner
//

TEST_MAIN(statusbar_widget_bridge, galaxy_widget_labels_test)
