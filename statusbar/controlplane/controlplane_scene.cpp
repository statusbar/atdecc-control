// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/controlplane/controlplane_scene.hpp"

#include "statusbar/controlplane/controlplane.hpp"

#include <cstdio>
#include <cstdlib>

namespace statusbar::controlplane {

Scene capture_scene(Instance const& instance, Selector const& mask, std::string name)
{
    Scene scene;
    scene.name = std::move(name);
    scene.mask = mask;
    scene.values.assign(instance.size(), 0);
    mask.for_each_set([&](ControlPointId h) { scene.values[h] = instance.read(h).reported; });
    return scene;
}

Selector recall_scene(Instance& instance, Scene const& scene, std::uint64_t now_ns)
{
    Selector clamped(instance.size());
    auto const& model = instance.model();
    scene.mask.for_each_set([&](ControlPointId h) {
        auto bits = scene.values[h];
        // Offline validation uses the model's range exactly as the
        // device would clamp — but only where the model declares one;
        // min == max means "no range authored", not "pin to a point".
        if (model.min(h) < model.max(h)) {
            auto v = value_double(bits);
            auto limited = v < model.min(h) ? model.min(h) : v > model.max(h) ? model.max(h) : v;
            if (limited != v) {
                clamped.set(h);
                bits = value_bits(limited);
            }
        }
        instance.set_local(h, bits, now_ns);
    });
    return clamped;
}

Selector diff_scene(Instance const& instance, Scene const& scene)
{
    Selector out(instance.size());
    scene.mask.for_each_set([&](ControlPointId h) {
        if (instance.read(h).reported != scene.values[h]) {
            out.set(h);
        }
    });
    return out;
}

std::string scene_to_text(Scene const& scene, PathAdaptor const& adaptor, std::string_view model_id)
{
    std::string out = "# statusbar-scene v1\n# name ";
    out += scene.name;
    out += "\n# model ";
    out += model_id.empty() ? "-" : std::string(model_id);
    out += "\n";
    scene.mask.for_each_set([&](ControlPointId h) {
        auto const& path = adaptor.address_of(h);
        if (path.empty()) {
            return;
        }
        char value[48];
        snprintf(value, sizeof value, "%.12g", value_double(scene.values[h]));
        out += path;
        out += '\t';
        out += value;
        out += '\n';
    });
    return out;
}

SceneResolution scene_from_text(std::string_view text, PathAdaptor const& adaptor)
{
    SceneResolution out;
    out.scene.mask = Selector(adaptor.handle_space());
    out.scene.values.assign(adaptor.handle_space(), 0);

    std::size_t at = 0;
    while (at < text.size()) {
        auto end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        auto line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty()) {
            continue;
        }
        if (line.front() == '#') {
            if (line.rfind("# name ", 0) == 0) {
                out.scene.name.assign(line.substr(7));
            } else if (line.rfind("# model ", 0) == 0 && line.substr(8) != "-") {
                out.model_id.assign(line.substr(8));
            }
            continue;
        }
        auto tab = line.find('\t');
        if (tab == std::string_view::npos) {
            out.missing.emplace_back(line);
            continue;
        }
        auto path = line.substr(0, tab);
        auto h = adaptor.resolve(path);
        if (!h) {
            out.missing.emplace_back(path);
            continue;
        }
        auto value_text = std::string(line.substr(tab + 1));
        out.scene.mask.set(*h);
        out.scene.values[*h] = value_bits(std::strtod(value_text.c_str(), nullptr));
        ++out.matched;
    }
    return out;
}

}  // namespace statusbar::controlplane
