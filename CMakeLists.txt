cmake_minimum_required(VERSION 3.31)

# ---- version --------------------------------------------------------------
# Single source of truth: the repo's latest release tag. Resolution order: 1.
# -DSTATUSBAR_ATDECC_CONTROL_VERSION=x.y.z (container builds resolve the tag on
# the host) 2. git describe on this tree (developer builds) 3. the fallback
# literal below (tarball builds; bump on release only if neither of the above is
# available). The version flows from here into the package metadata and the
# proxy entity's firmware_version (STATUSBAR_ATDECC_CONTROL_VERSION compile
# definition).
if(NOT STATUSBAR_ATDECC_CONTROL_VERSION)
  execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" describe --tags --abbrev=0
            --match "[0-9]*.[0-9]*.[0-9]*"
    OUTPUT_VARIABLE STATUSBAR_ATDECC_CONTROL_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT STATUSBAR_ATDECC_CONTROL_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  set(STATUSBAR_ATDECC_CONTROL_VERSION 2.1.0)
endif()

project(
  statusbar-atdecc-control
  VERSION ${STATUSBAR_ATDECC_CONTROL_VERSION}
  LANGUAGES CXX)
message(STATUS "statusbar-atdecc-control version: ${PROJECT_VERSION}")

# Baked into the proxy entity's ENTITY descriptor firmware_version.
add_compile_definitions(STATUSBAR_ATDECC_CONTROL_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
enable_testing()

# Modules register bench tools into this aggregate target. The outer aggregate
# build supplies a single shared target instead.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  add_custom_target(run-all-benches)
endif()

# Dependency packages — must be installed and discoverable. statusbar-crypto and
# statusbar-audio arrive transitively through statusbar-avb's package config;
# only what this package links directly is listed here.
find_package(statusbar-core REQUIRED)
find_package(statusbar-avb REQUIRED)

add_subdirectory(statusbar)

# Uniform statusbar- prefixed on-disk names for installed executables
# (statusbar_prefix_tool_names in cmake/module.cmake).
statusbar_prefix_tool_names()

# ---- install + export ----
# Standalone install/export boilerplate lives in
# statusbar_package_install_export (cmake/module.cmake); no-op under an outer
# aggregate build.
statusbar_package_install_export(NAME atdecc-control)

# ---- packaging (CPack: TGZ + ZIP, plus DEB on Linux) ----
# Standalone builds only — run `cpack` from the build directory.
if(NOT STATUSBAR_AGGREGATE_BUILD)
  statusbar_package_cpack_defaults(
    NAME atdecc-control NO_RPM DESCRIPTION
    "ATDECC control-point data model and GALAXY control proxy")
  # Package-specific component wording (overrides the shared defaults).
  set(CPACK_COMPONENT_RUNTIME_DESCRIPTION
      "GALAXY proxy entity and control tools (statically linked)")
  set(CPACK_COMPONENT_DEVEL_DESCRIPTION
      "Static libraries, headers, and CMake package config")
  set(CPACK_PACKAGE_CONTACT "Jeff Koftinoff <jeff.koftinoff@statusbar.com>")
  set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
  # -dev pulls the dep -dev packages so its cmake config's IMPORTED targets
  # resolve; >= so auto-incremented Debian revisions still satisfy. Floors track
  # the versions this build was actually made against (find_package sets
  # statusbar-core_VERSION / statusbar-avb_VERSION; crypto and audio are not
  # find_package()d directly — the suite versions in lockstep, so core's version
  # is their floor too).
  set(CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS
      "statusbar-core-dev (>= ${statusbar-core_VERSION}), statusbar-crypto-dev (>= ${statusbar-core_VERSION}), statusbar-audio-dev (>= ${statusbar-core_VERSION}), statusbar-avb-dev (>= ${statusbar-avb_VERSION})"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # libcap2-bin provides `setcap` for the postinst raw-ethernet grant to
    # statusbar-galaxy-proxy (ADP/AECP need AF_PACKET without root).
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "libcap2-bin")
    set(SETCAP_BINDIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst.in
                   ${CMAKE_CURRENT_BINARY_DIR}/packaging/deb/postinst @ONLY)
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/deb/postinst")
  endif()
  include(CPack)
endif()
