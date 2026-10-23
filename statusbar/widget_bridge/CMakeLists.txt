# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  widget_bridge
  SOURCES
  galaxy_widget_labels.cpp
  offline_scenes.cpp
  offline_service.cpp
  widget_bridge.cpp
  widget_controls.cpp
  widget_json.cpp
  DEPS
  statusbar-galaxy_proxy
  statusbar::atdecc
  statusbar::atdecc_tools
  statusbar::http
  statusbar::nanoavb
  statusbar::net
  TESTS
  galaxy_widget_labels_test.cpp
  offline_scenes_test.cpp
  widget_bridge_test.cpp
  widget_json_test.cpp)

add_executable(widget_bridge_tool
               ${CMAKE_CURRENT_LIST_DIR}/widget_bridge_tool.cpp)
target_link_libraries(widget_bridge_tool PRIVATE statusbar)
set_target_properties(widget_bridge_tool PROPERTIES OUTPUT_NAME
                                                    statusbar-widget-bridge)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    widget_bridge_tool)

# The reference web app (PLAN §12.5), installed so a deployed node can serve it
# directly: statusbar-widget-bridge eth0 \ --manifest
# /usr/local/share/statusbar-atdecc-control/widget-bridge-site/manifest.toml
if(NOT STATUSBAR_AGGREGATE_BUILD)
  install(
    FILES ${CMAKE_CURRENT_LIST_DIR}/site/index.html
          ${CMAKE_CURRENT_LIST_DIR}/site/manifest.toml
    DESTINATION
      ${CMAKE_INSTALL_DATADIR}/statusbar-atdecc-control/widget-bridge-site
    COMPONENT runtime)
endif()
