// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Scene authoring against the GALAXY namespace (M7): offline against the
// schema (a virtual entity — no device needed), or live via the M4
// mirror. Scene files bind to GALAXY paths, never handles.
//
//   statusbar-galaxy-scene --author <scene> <pattern...>
//       author a scene from the schema defaults for the selected points
//       (scalar points only; strings are not scene material)
//   statusbar-galaxy-scene --validate <scene>
//       resolve a scene file offline: matched / missing / unmatched report
//   statusbar-galaxy-scene --capture <scene> --host <h> [pattern...]
//       capture the live values of the selected points (default: all
//       reported scalars)
//   statusbar-galaxy-scene --diff <scene> --host <h>
//       list points whose live value differs from the scene
//   statusbar-galaxy-scene --recall <scene> --host <h>
//       set every differing scene value on the device
//
// Patterns use the schema pattern grammar: `*` any segment, `#` numeric.
// A pattern may also be the device's `@N` alias form, selecting that one
// point.

#include "statusbar/controlplane/controlplane.hpp"
#include "statusbar/controlplane/controlplane_scene.hpp"
#include "statusbar/controlplane_devices/galaxy_ascii.hpp"
#include "statusbar/controlplane_devices/galaxy_client.hpp"
#include "statusbar/controlplane_devices/galaxy_mirror.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::controlplane;
using namespace statusbar::controlplane_devices;

namespace {

std::uint64_t now_ns()
{
    return std::uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

int usage()
{
    fprintf(
        stderr,
        "usage: statusbar-galaxy-scene --author <scene> <pattern...>\n"
        "       statusbar-galaxy-scene --validate <scene>\n"
        "       statusbar-galaxy-scene --capture <scene> --host <h> [pattern...]\n"
        "       statusbar-galaxy-scene --diff <scene> --host <h>\n"
        "       statusbar-galaxy-scene --recall <scene> --host <h>\n");
    return 2;
}

/// Scalar (numeric or boolean) points are scene material; free strings
/// (names, dates, log text) are not.
bool is_scalar_point(GalaxyNamespace const& ns, ControlPointId h)
{
    auto const& def = ns.defaults[h];
    if (def == "true" || def == "false") {
        return true;
    }
    if (def.empty()) {
        return false;
    }
    char* end = nullptr;
    (void)std::strtod(def.c_str(), &end);
    return end == def.c_str() + def.size();
}

Selector select_patterns(GalaxyNamespace const& ns, std::vector<std::string> const& patterns)
{
    Selector out(ns.size());
    if (patterns.empty()) {
        out.set_all();
    }
    for (auto const& p : patterns) {
        out |= ns.resolve_selection(p);
    }
    // Writable scalars only.
    Selector scalars(ns.size());
    for (ControlPointId h = 0; h < ns.size(); ++h) {
        if (is_scalar_point(ns, h) && !ns.model->read_only(h)) {
            scalars.set(h);
        }
    }
    out &= scalars;
    return out;
}

bool read_text_file(std::string const& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    char buf[16384];
    std::size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
    }
    fclose(f);
    return true;
}

bool write_text_file(std::string const& path, std::string const& text)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    auto n = fwrite(text.data(), 1, text.size(), f);
    return fclose(f) == 0 && n == text.size();
}

/// Populates the mirror from the live device (pattern gets, drain).
bool dump_live(GalaxyClient& client, GalaxyMirror& mirror)
{
    static char const* CATEGORIES[] = {"/processing/.*", "/device/.*", "/system/.*", "/project/.*", "/entity/.*", "/status/.*"};
    for (auto* pattern : CATEGORIES) {
        if (!client.send(galaxy_get(pattern))) {
            return false;
        }
        client.drain(1000, 30000, [&](std::string_view line) { mirror.consume_line(line, now_ns()); });
    }
    return true;
}

std::string format_for_device(GalaxyNamespace const& ns, ControlPointId h, double value)
{
    auto const& def = ns.defaults[h];
    if (def == "true" || def == "false") {
        return value != 0.0 ? "true" : "false";
    }
    char buf[48];
    snprintf(buf, sizeof buf, "%g", value);
    return buf;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        return usage();
    }
    std::string mode = argv[1];
    std::string scene_path = argv[2];
    std::string host;
    std::vector<std::string> patterns;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else {
            patterns.emplace_back(argv[i]);
        }
    }

    auto ns = build_galaxy_816_namespace();

    if (mode == "--author") {
        // A virtual entity seeded from the schema defaults — offline
        // authoring with no device present (PLAN §6.4).
        Instance virt(ns.model, Role::entity);
        for (ControlPointId h = 0; h < ns.size(); ++h) {
            if (is_scalar_point(ns, h)) {
                auto const& def = ns.defaults[h];
                double v = def == "true" ? 1.0 : def == "false" ? 0.0 : std::strtod(def.c_str(), nullptr);
                virt.set_local(h, value_bits(v), 0);
            }
        }
        auto mask = select_patterns(ns, patterns);
        auto scene = capture_scene(virt, mask, scene_path);
        if (!write_text_file(scene_path, scene_to_text(scene, ns.adaptor, "galaxy-816"))) {
            fprintf(stderr, "cannot write %s\n", scene_path.c_str());
            return 1;
        }
        printf("%s: %zu points authored from defaults\n", scene_path.c_str(), mask.count());
        return 0;
    }

    std::string text;
    if (mode != "--capture" && !read_text_file(scene_path, text)) {
        fprintf(stderr, "cannot read %s\n", scene_path.c_str());
        return 1;
    }

    if (mode == "--validate") {
        auto resolved = scene_from_text(text, ns.adaptor);
        printf(
            "scene '%s' (model %s): %zu matched, %zu missing, %zu unmatched\n",
            resolved.scene.name.c_str(),
            resolved.model_id.empty() ? "-" : resolved.model_id.c_str(),
            resolved.matched,
            resolved.missing.size(),
            ns.size() - resolved.matched);
        for (auto const& m : resolved.missing) {
            printf("missing: %s\n", m.c_str());
        }
        return resolved.missing.empty() ? 0 : 1;
    }

    // Live modes.
    if (host.empty()) {
        return usage();
    }
    GalaxyClient client;
    if (!client.connect(host)) {
        fprintf(stderr, "connect to %s failed: %s\n", host.c_str(), client.error().c_str());
        return 1;
    }
    GalaxyMirror mirror(ns);
    if (!dump_live(client, mirror)) {
        fprintf(stderr, "state request failed: %s\n", client.error().c_str());
        return 1;
    }

    if (mode == "--capture") {
        auto mask = select_patterns(ns, patterns);
        mask &= mirror.reported();
        auto scene = capture_scene(mirror.instance(), mask, scene_path);
        if (!write_text_file(scene_path, scene_to_text(scene, ns.adaptor, "galaxy-816"))) {
            fprintf(stderr, "cannot write %s\n", scene_path.c_str());
            return 1;
        }
        printf("%s: %zu live points captured\n", scene_path.c_str(), mask.count());
        return 0;
    }

    auto resolved = scene_from_text(text, ns.adaptor);
    auto drift = diff_scene(mirror.instance(), resolved.scene);

    if (mode == "--diff") {
        drift.for_each_set([&](ControlPointId h) {
            printf(
                "%s\tscene=%s\tlive=%s\n",
                ns.adaptor.address_of(h).c_str(),
                format_for_device(ns, h, value_double(resolved.scene.values[h])).c_str(),
                mirror.reported().test(h) ? mirror.value_of(h).c_str() : "<unreported>");
        });
        fprintf(
            stderr,
            "%zu of %zu scene points differ; %zu missing from this model\n",
            drift.count(),
            resolved.matched,
            resolved.missing.size());
        return 0;
    }

    if (mode == "--recall") {
        std::size_t sent = 0;
        drift.for_each_set([&](ControlPointId h) {
            auto value = format_for_device(ns, h, value_double(resolved.scene.values[h]));
            if (client.send(galaxy_set(ns.adaptor.address_of(h), value))) {
                ++sent;
            }
        });
        // The device does not echo sets back on the setting connection,
        // and applies sets asynchronously — an immediate re-read can still
        // serve pre-set values (both verified live, Q1 fw 2.10.0). Let the
        // device settle, then re-request state for the residual diff.
        (void)client.poll_lines(750, [](std::string_view) {});
        if (!dump_live(client, mirror)) {
            fprintf(stderr, "post-recall state request failed: %s\n", client.error().c_str());
            return 1;
        }
        auto remaining = diff_scene(mirror.instance(), resolved.scene);
        printf("recalled %zu points; %zu still differ (device clamps or rejects)\n", sent, remaining.count());
        for (auto const& m : resolved.missing) {
            printf("missing: %s\n", m.c_str());
        }
        return 0;
    }

    return usage();
}
