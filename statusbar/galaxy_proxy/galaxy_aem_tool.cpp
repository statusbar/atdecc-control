// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Emits the galaxy_proxy entity model as a .aem blob.
//
//   statusbar-galaxy-aem <out.aem>     write the blob
//   statusbar-galaxy-aem --summary     print descriptor counts and binding
//                                      coverage without writing

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/controlplane_devices/galaxy_schema.hpp"
#include "statusbar/galaxy_proxy/galaxy_aem_model.hpp"

#include <cstdio>
#include <string>

using namespace statusbar;
using namespace statusbar::atdecc::aem;

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: statusbar-galaxy-aem <out.aem> | --summary\n");
        return 2;
    }
    auto ns = controlplane_devices::build_galaxy_816_namespace();
    auto model = galaxy_proxy::build_galaxy_proxy_aem_model(ns);

    fprintf(
        stderr,
        "descriptors: %zu total — ENTITY %zu, CONFIGURATION %zu, AUDIO_UNIT %zu, CONTROL %zu, "
        "CONTROL_BLOCK %zu, MATRIX %zu, MATRIX_SIGNAL %zu, SIGNAL_SELECTOR %zu\n",
        model.descriptors.size(),
        model.count_of(DESCRIPTOR_ENTITY),
        model.count_of(DESCRIPTOR_CONFIGURATION),
        model.count_of(DESCRIPTOR_AUDIO_UNIT),
        model.count_of(DESCRIPTOR_CONTROL),
        model.count_of(DESCRIPTOR_CONTROL_BLOCK),
        model.count_of(DESCRIPTOR_MATRIX),
        model.count_of(DESCRIPTOR_MATRIX_SIGNAL),
        model.count_of(DESCRIPTOR_SIGNAL_SELECTOR));
    fprintf(
        stderr,
        "bindings: %zu of %zu schema points have an AEM projection; blob %zu bytes\n",
        model.binding.bound_count(),
        ns.size(),
        model.blob.size());

    std::string arg = argv[1];
    if (arg == "--summary") {
        return 0;
    }
    FILE* f = fopen(arg.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", arg.c_str());
        return 1;
    }
    auto written = fwrite(model.blob.data(), 1, model.blob.size(), f);
    fclose(f);
    if (written != model.blob.size()) {
        fprintf(stderr, "short write to %s\n", arg.c_str());
        return 1;
    }
    printf("%s: %zu bytes\n", arg.c_str(), model.blob.size());
    return 0;
}
