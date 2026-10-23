# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  controlplane
  DEPS
  statusbar::container
  SOURCES
  controlplane_adaptor.cpp
  controlplane_aem_binding.cpp
  controlplane_instance.cpp
  controlplane_model.cpp
  controlplane_scene.cpp
  controlplane_symbols.cpp
  controlplane_trie.cpp
  TESTS
  controlplane_test.cpp
  controlplane_adaptor_test.cpp
  controlplane_aem_binding_test.cpp
  controlplane_bitset_test.cpp
  controlplane_model_test.cpp
  controlplane_instance_test.cpp
  controlplane_scene_test.cpp
  controlplane_tick_test.cpp
  controlplane_trie_test.cpp)

# Design-justification benchmarks (docs/PLAN.md §4.4, §4.7): the measured
# fan-out and address-selection numbers are regression tests — they should not
# silently rot.
add_executable(controlplane_fanout_bench_tool
               ${CMAKE_CURRENT_LIST_DIR}/controlplane_fanout_bench_tool.cpp)
target_link_libraries(controlplane_fanout_bench_tool PRIVATE statusbar)
set_target_properties(
  controlplane_fanout_bench_tool PROPERTIES OUTPUT_NAME
                                            statusbar-controlplane-fanout-bench)

add_custom_target(
  run-controlplane-fanout-bench
  COMMAND $<TARGET_FILE:controlplane_fanout_bench_tool>
  DEPENDS controlplane_fanout_bench_tool
  USES_TERMINAL VERBATIM)
add_dependencies(run-all-benches run-controlplane-fanout-bench)

# Regression gate: the real Instance/tick() fan-out must stay within an order of
# magnitude of the raw-bitset reference measured in the same binary. The bench
# only enforces in optimized builds; in Debug it degrades to a smoke test.
add_test(NAME statusbar/controlplane/fanout_bench_check
         COMMAND controlplane_fanout_bench_tool --check)

# The selection bench compares trie/linear/regex against a SQLite baseline; it
# only builds where SQLite3 is available (macOS SDK and Debian both ship it).
find_package(SQLite3)
if(SQLite3_FOUND)
  add_executable(controlplane_select_bench_tool
                 ${CMAKE_CURRENT_LIST_DIR}/controlplane_select_bench_tool.cpp)
  target_link_libraries(controlplane_select_bench_tool PRIVATE statusbar
                                                               SQLite3::SQLite3)
  set_target_properties(
    controlplane_select_bench_tool
    PROPERTIES OUTPUT_NAME statusbar-controlplane-select-bench)

  add_custom_target(
    run-controlplane-select-bench
    COMMAND $<TARGET_FILE:controlplane_select_bench_tool>
    DEPENDS controlplane_select_bench_tool
    USES_TERMINAL VERBATIM)
  add_dependencies(run-all-benches run-controlplane-select-bench)

  # Regression gate: PathAdaptor must select 256 of 10k control points in under
  # 10 us (the M2 acceptance criterion; enforced in optimized builds,
  # correctness of the hit count enforced always).
  add_test(NAME statusbar/controlplane/select_bench_check
           COMMAND controlplane_select_bench_tool --check)
endif()
