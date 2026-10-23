// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Dumps the generated GALAXY 816 namespace.
//
//   statusbar-galaxy-namespace            path<TAB>default<TAB>ro per line
//   statusbar-galaxy-namespace --describe the full `?` describe response
//
// The default listing feeds python/galaxy/check_namespace.py, which
// cross-checks the generated namespace against the raw rows extracted from
// both editions of the programming guide; --describe feeds
// check_describe_roundtrip.py, which parses the response as both a Python
// literal and a JavaScript expression.

#include "statusbar/controlplane_devices/galaxy_schema.hpp"

#include <cstdio>
#include <string>

using namespace statusbar;

int main(int argc, char** argv)
{
    bool describe = argc > 1 && std::string(argv[1]) == "--describe";
    auto ns = controlplane_devices::build_galaxy_816_namespace();
    if (describe) {
        controlplane::Selector all(ns.size());
        all.set_all();
        auto response = controlplane_devices::galaxy_describe_response(ns, all);
        fwrite(response.data(), 1, response.size(), stdout);
        fputc('\n', stdout);
        return 0;
    }
    for (controlplane::ControlPointId h = 0; h < ns.size(); ++h) {
        printf("%s\t%s\t%s\n", ns.adaptor.address_of(h).c_str(), ns.defaults[h].c_str(), ns.model->read_only(h) ? "ro" : "rw");
    }
    return 0;
}
