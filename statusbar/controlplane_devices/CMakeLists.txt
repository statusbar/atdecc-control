# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  controlplane_devices
  SOURCES
  galaxy_ascii.cpp
  galaxy_client.cpp
  galaxy_mirror.cpp
  galaxy_schema.cpp
  nadia_ascii.cpp
  nadia_discover.cpp
  nadia_mirror.cpp
  nadia_schema.cpp
  DEPS
  statusbar-controlplane
  statusbar::net
  TESTS
  controlplane_devices_test.cpp
  galaxy_ascii_test.cpp
  galaxy_client_test.cpp
  galaxy_mirror_test.cpp
  galaxy_schema_test.cpp
  nadia_ascii_test.cpp
  nadia_discover_test.cpp
  nadia_mirror_test.cpp
  nadia_schema_test.cpp)

add_executable(galaxy_namespace_tool
               ${CMAKE_CURRENT_LIST_DIR}/galaxy_namespace_tool.cpp)
target_link_libraries(galaxy_namespace_tool PRIVATE statusbar)
set_target_properties(galaxy_namespace_tool
                      PROPERTIES OUTPUT_NAME statusbar-galaxy-namespace)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    galaxy_namespace_tool)

# M3 acceptance gates: the generated namespace must match the path shapes
# extracted from both editions of the programming guide, and the `?` describe
# response must round-trip as a Python literal and (when node is available) a
# JavaScript expression, every value quoted.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME statusbar/controlplane_devices/galaxy_namespace_check
    COMMAND
      Python3::Interpreter
      ${PROJECT_SOURCE_DIR}/python/galaxy/check_namespace.py --tool
      $<TARGET_FILE:galaxy_namespace_tool>)
  add_test(
    NAME statusbar/controlplane_devices/galaxy_ranges_check
    COMMAND Python3::Interpreter
            ${PROJECT_SOURCE_DIR}/python/galaxy/fold_describes.py --check)
  add_test(
    NAME statusbar/controlplane_devices/galaxy_describe_roundtrip
    COMMAND
      Python3::Interpreter
      ${PROJECT_SOURCE_DIR}/python/galaxy/check_describe_roundtrip.py
      $<TARGET_FILE:galaxy_namespace_tool>)
endif()
