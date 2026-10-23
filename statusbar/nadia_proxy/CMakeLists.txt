# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  nadia_proxy
  SOURCES
  nadia_aem_model.cpp
  nadia_proxy.cpp
  DEPS
  statusbar-controlplane_devices
  statusbar::avb_entity
  statusbar::nanoavb
  TESTS
  nadia_aem_model_test.cpp
  nadia_proxy_loopback_test.cpp)

add_executable(nadia_proxy_tool ${CMAKE_CURRENT_LIST_DIR}/nadia_proxy_tool.cpp)
target_link_libraries(nadia_proxy_tool PRIVATE statusbar)
set_target_properties(nadia_proxy_tool PROPERTIES OUTPUT_NAME
                                                  statusbar-nadia-cp-proxy)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS nadia_proxy_tool)
