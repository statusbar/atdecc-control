# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

# Package INTERFACE library, include path and the global accumulation properties
# (statusbar_package_module_init in cmake/module.cmake).
statusbar_package_module_init()

# Module subdirectories — dependency order. controlplane is the protocol-free
# core: it must never grow a dependency on statusbar-avb, so it stays usable in
# a non-AVB device and its tests stay fast. (The AEM binding adaptor lives
# inside it — pure tuple bookkeeping, no AVB headers; the .aem blob tooling that
# DOES speak avb moved to statusbar-avb's nanoavb module.)
add_subdirectory(controlplane)
add_subdirectory(controlplane_devices)
add_subdirectory(galaxy_proxy)
add_subdirectory(nadia_proxy)
add_subdirectory(controlplane_tools)
add_subdirectory(widget_bridge)

# Populate the package INTERFACE library — skipped under the outer aggregate
# build, which populates a single shared `statusbar` itself.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  target_link_libraries(
    statusbar
    INTERFACE statusbar-controlplane
              statusbar-controlplane_devices
              statusbar-galaxy_proxy
              statusbar-nadia_proxy
              statusbar-controlplane_tools
              statusbar-widget_bridge
              statusbar::http
              statusbar::atdecc
              statusbar::atdecc_tools
              statusbar::avb_entity
              statusbar::avtp
              statusbar::args
              statusbar::buffer
              statusbar::config
              statusbar::container
              statusbar::ieee
              statusbar::ip
              statusbar::net
              statusbar::status
              statusbar::test)
endif()

# Standalone per-package test binary (statusbar_package_test_binary in
# cmake/module.cmake); the outer aggregate build builds one combined
# statusbar_test from every aggregated package instead.
statusbar_package_test_binary()
