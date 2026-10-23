// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for scenes: capture/recall/diff against a virtual entity (an
// Instance with no transport), clamping to the model's declared ranges,
// and symbol-bound text round-trips with matched/missing classification.

#include "statusbar/controlplane/controlplane_scene.hpp"

#include "statusbar/controlplane/controlplane.hpp"
#include "statusbar/test/test.hpp"

#include <memory>
#include <string>

using namespace statusbar;
using namespace statusbar::controlplane;

namespace {

struct Virtual
{
    std::shared_ptr<Model const> model;
    PathAdaptor adaptor;

    Virtual()
    {
        ModelBuilder mb;
        PathAdaptorBuilder pb(4);
        ControlPointDesc gain;
        gain.min = -90.0;
        gain.max = 10.0;
        gain.step = 0.1;
        pb.map("/input/1/gain", mb.add(gain));
        pb.map("/input/2/gain", mb.add(gain));
        pb.map("/input/1/mute", mb.add(ControlPointDesc{}));  // no declared range
        pb.map("/input/1/name", mb.add(ControlPointDesc{}));
        model = mb.build();
        adaptor = pb.build();
    }
};

}  // namespace

TEST(scene, capture_recall_diff_on_virtual_entity)
{
    Virtual v;
    Instance live(v.model, Role::entity);  // virtual entity: no transport
    live.set_local(0, value_bits(-6.0), 1);
    live.set_local(1, value_bits(3.0), 2);
    live.set_local(2, value_bits(1.0), 3);

    Selector mask(4);
    mask.set(0);
    mask.set(2);
    auto scene = capture_scene(live, mask, "show A");
    EXPECT_TRUE(scene.name == "show A");
    EXPECT_EQ(scene.mask.count(), 2u);
    EXPECT_EQ(value_double(scene.values[0]), -6.0);

    // Live moves on; diff sees exactly the masked drift.
    live.set_local(0, value_bits(-20.0), 4);
    live.set_local(1, value_bits(5.0), 5);  // outside mask: invisible to diff
    auto drift = diff_scene(live, scene);
    EXPECT_EQ(drift.count(), 1u);
    EXPECT_TRUE(drift.test(0));

    // Recall is a masked write: point 1 keeps its live value.
    auto clamped = recall_scene(live, scene, 6);
    EXPECT_EQ(clamped.count(), 0u);
    EXPECT_EQ(value_double(live.read(0).reported), -6.0);
    EXPECT_EQ(value_double(live.read(1).reported), 5.0);
    EXPECT_EQ(diff_scene(live, scene).count(), 0u);
}

TEST(scene, recall_clamps_to_declared_ranges_only)
{
    Virtual v;
    Instance live(v.model, Role::entity);

    Scene scene;
    scene.name = "hot";
    scene.mask = Selector(4);
    scene.values.assign(4, 0);
    scene.mask.set(0);
    scene.values[0] = value_bits(99.0);  // beyond gain max 10
    scene.mask.set(2);
    scene.values[2] = value_bits(1234.0);  // mute has no declared range

    auto clamped = recall_scene(live, scene, 1);
    EXPECT_EQ(clamped.count(), 1u);
    EXPECT_TRUE(clamped.test(0));
    EXPECT_EQ(value_double(live.read(0).reported), 10.0);    // clamped
    EXPECT_EQ(value_double(live.read(2).reported), 1234.0);  // untouched
}

TEST(scene, text_round_trip_and_resolution_report)
{
    Virtual v;
    Instance live(v.model, Role::entity);
    live.set_local(0, value_bits(-6.5), 1);
    live.set_local(2, value_bits(1.0), 2);

    Selector mask(4);
    mask.set(0);
    mask.set(2);
    auto scene = capture_scene(live, mask, "show B");
    auto text = scene_to_text(scene, v.adaptor, "fp123");
    EXPECT_TRUE(text.find("# statusbar-scene v1") == 0u);
    EXPECT_TRUE(text.find("/input/1/gain\t-6.5") != std::string::npos);

    auto resolved = scene_from_text(text, v.adaptor);
    EXPECT_TRUE(resolved.scene.name == "show B");
    EXPECT_TRUE(resolved.model_id == "fp123");
    EXPECT_EQ(resolved.matched, 2u);
    EXPECT_EQ(resolved.missing.size(), 0u);
    EXPECT_TRUE(resolved.scene.mask == scene.mask);
    EXPECT_EQ(value_double(resolved.scene.values[0]), -6.5);

    // A scene authored against a different model: the unknown address is
    // reported missing, the rest still matches; unmatched is the mask
    // complement (a report, not an error path).
    auto foreign = text + "/input/9/gain\t0\n";
    auto partial = scene_from_text(foreign, v.adaptor);
    EXPECT_EQ(partial.matched, 2u);
    EXPECT_EQ(partial.missing.size(), 1u);
    EXPECT_TRUE(partial.missing[0] == "/input/9/gain");
}

//
// Test Runner
//

TEST_MAIN(statusbar_controlplane, controlplane_scene_test)
