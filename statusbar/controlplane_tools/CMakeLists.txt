# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  controlplane_tools
  INTERFACE
  DEPS
  statusbar-controlplane
  statusbar-controlplane_devices
  TESTS
  controlplane_tools_test.cpp)

add_executable(galaxy_mirror_tool
               ${CMAKE_CURRENT_LIST_DIR}/galaxy_mirror_tool.cpp)
target_link_libraries(galaxy_mirror_tool PRIVATE statusbar)
set_target_properties(galaxy_mirror_tool PROPERTIES OUTPUT_NAME
                                                    statusbar-galaxy-mirror)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    galaxy_mirror_tool)

add_executable(galaxy_scene_tool
               ${CMAKE_CURRENT_LIST_DIR}/galaxy_scene_tool.cpp)
target_link_libraries(galaxy_scene_tool PRIVATE statusbar)
set_target_properties(galaxy_scene_tool PROPERTIES OUTPUT_NAME
                                                   statusbar-galaxy-scene)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS galaxy_scene_tool)

add_executable(nadia_mirror_tool
               ${CMAKE_CURRENT_LIST_DIR}/nadia_mirror_tool.cpp)
target_link_libraries(nadia_mirror_tool PRIVATE statusbar)
set_target_properties(nadia_mirror_tool PROPERTIES OUTPUT_NAME
                                                   statusbar-nadia-mirror)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS nadia_mirror_tool)
