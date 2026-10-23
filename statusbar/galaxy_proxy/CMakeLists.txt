# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  galaxy_proxy
  SOURCES
  galaxy_aem_model.cpp
  galaxy_proxy.cpp
  DEPS
  statusbar-controlplane_devices
  statusbar::avb_entity
  statusbar::nanoavb
  TESTS
  galaxy_proxy_test.cpp
  galaxy_aem_model_test.cpp
  galaxy_proxy_loopback_test.cpp
  galaxy_model_store_test.cpp)

add_executable(galaxy_aem_tool ${CMAKE_CURRENT_LIST_DIR}/galaxy_aem_tool.cpp)
target_link_libraries(galaxy_aem_tool PRIVATE statusbar)
set_target_properties(galaxy_aem_tool PROPERTIES OUTPUT_NAME
                                                 statusbar-galaxy-aem)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS galaxy_aem_tool)

add_executable(galaxy_model_verify_tool
               ${CMAKE_CURRENT_LIST_DIR}/galaxy_model_verify_tool.cpp)
target_link_libraries(galaxy_model_verify_tool PRIVATE statusbar
                                                       statusbar::atdecc_tools)
set_target_properties(galaxy_model_verify_tool
                      PROPERTIES OUTPUT_NAME statusbar-galaxy-model-verify)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS
                                    galaxy_model_verify_tool)

add_executable(galaxy_proxy_tool
               ${CMAKE_CURRENT_LIST_DIR}/galaxy_proxy_tool.cpp)
target_link_libraries(galaxy_proxy_tool PRIVATE statusbar)
set_target_properties(galaxy_proxy_tool PROPERTIES OUTPUT_NAME
                                                   statusbar-galaxy-proxy)
set_property(GLOBAL APPEND PROPERTY STATUSBAR_INSTALL_TARGETS galaxy_proxy_tool)
