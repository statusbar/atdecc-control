// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include "statusbar/controlplane/controlplane_adaptor.hpp"
#include "statusbar/controlplane/controlplane_instance.hpp"

#include <string>
#include <string_view>
#include <vector>

// Scenes (docs/PLAN.md §4.3, §6.4): a scene is a mask plus values, since
// scenes are partial. Scene recall is a masked write; scene-versus-live
// diff is AND plus a value compare. The three value layers are the model's
// defaults (static), the scene (persistent, authored offline — this is the
// document), and reported (volatile, live only — never persisted).
//
// Persistence binds to symbols — here, adaptor addresses — never to raw
// handles (§6.3), so a scene file survives model regeneration as long as
// the addresses resolve. Deployment classifies the residue: matched,
// missing (in scene, not in the address space), unmatched (in the address
// space, not in the scene) — a report, not an error path.

namespace statusbar::controlplane {

struct Scene
{
    std::string name;
    Selector mask;                      ///< which points the scene covers
    std::vector<std::uint64_t> values;  ///< dense; valid where mask is set

    std::size_t size() const noexcept { return mask.size(); }
};

/// Snapshots the instance's reported values for every point in @p mask.
Scene capture_scene(Instance const& instance, Selector const& mask, std::string name);

/// Masked write of the scene into the instance (local sets — authoritative
/// on an entity-role instance, desired+pending on a controller). Values
/// are clamped to the model's min/max where the model declares a range
/// (min < max); the returned selector lists the points that were clamped —
/// per-point failures are collected, never aborted on (§6.4).
Selector recall_scene(Instance& instance, Scene const& scene, std::uint64_t now_ns);

/// Points in the scene's mask whose reported value differs from the scene.
Selector diff_scene(Instance const& instance, Scene const& scene);

/// Serializes a scene bound to adaptor addresses:
///
///   # statusbar-scene v1
///   # name <name>
///   # model <model_id>
///   /path<TAB>value
///
/// Points whose address is empty (unmapped handles) are skipped.
std::string scene_to_text(Scene const& scene, PathAdaptor const& adaptor, std::string_view model_id);

struct SceneResolution
{
    Scene scene;
    std::string model_id;              ///< the file's "# model" header, "" if absent
    std::vector<std::string> missing;  ///< file addresses the adaptor cannot resolve
    std::size_t matched = 0;           ///< addresses resolved into the mask
};

/// Parses and resolves a scene file against an address space. Unmatched
/// points (in the address space but not the scene) are the complement of
/// the returned mask. Malformed lines land in `missing`.
SceneResolution scene_from_text(std::string_view text, PathAdaptor const& adaptor);

}  // namespace statusbar::controlplane
