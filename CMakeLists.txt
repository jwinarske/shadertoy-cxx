# SPDX-License-Identifier: MIT
# Copyright (c) 2026 shadertoy-cxx contributors

cmake_minimum_required(VERSION 3.16)

project(shadertoy-cxx
    VERSION 0.3.0
    DESCRIPTION "Platform-agnostic Shadertoy renderer (OpenGL ES 3 + Vulkan)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ── Back-end selection (independently gated) ──────────────────────────────────
# AUTO  = compile the back-end if its library is present (default)
# ON    = require it (configure error if missing)
# OFF   = exclude it
set(SHADERTOY_GL "AUTO" CACHE STRING "OpenGL ES 3.0 back-end: AUTO, ON, or OFF")
set(SHADERTOY_VULKAN "AUTO" CACHE STRING "Vulkan back-end: AUTO, ON, or OFF")
set_property(CACHE SHADERTOY_GL PROPERTY STRINGS AUTO ON OFF)
set_property(CACHE SHADERTOY_VULKAN PROPERTY STRINGS AUTO ON OFF)
option(SHADERTOY_BUILD_EXAMPLES "Build the standalone offscreen example" OFF)

find_package(PkgConfig QUIET)

# OpenGL ES 3.0 (libglesv2 via pkg-config).
set(HAVE_GL 0)
if(NOT SHADERTOY_GL STREQUAL "OFF")
  if(PkgConfig_FOUND)
    pkg_check_modules(GLESV2 IMPORTED_TARGET glesv2)
  endif()
  if(GLESV2_FOUND)
    set(HAVE_GL 1)
  elseif(SHADERTOY_GL STREQUAL "ON")
    message(FATAL_ERROR "SHADERTOY_GL=ON but glesv2 (pkg-config) was not found")
  endif()
endif()

# Vulkan.
set(HAVE_VULKAN 0)
if(NOT SHADERTOY_VULKAN STREQUAL "OFF")
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    set(HAVE_VULKAN 1)
  elseif(SHADERTOY_VULKAN STREQUAL "ON")
    message(FATAL_ERROR "SHADERTOY_VULKAN=ON but Vulkan was not found")
  endif()
endif()

# ── Generated config header ───────────────────────────────────────────────────
set(SHADERTOY_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(SHADERTOY_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(SHADERTOY_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(SHADERTOY_VERSION_STRING ${PROJECT_VERSION})
set(SHADERTOY_HAVE_GL ${HAVE_GL})
set(SHADERTOY_HAVE_VULKAN ${HAVE_VULKAN})
set(SHADERTOY_HAVE_JSON 1)  # loader uses the vendored nlohmann/json
set(SHADERTOY_SHADER_DIR ${CMAKE_INSTALL_FULL_DATADIR}/shadertoy-cxx/shaders)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/shadertoy/config.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/shadertoy/config.hpp
  @ONLY)

# ── Library ───────────────────────────────────────────────────────────────────
# Core (always): model, JSON loader (nlohmann/json), playlist, source wrappers.
set(SHADERTOY_SOURCES
    src/inputs.cpp
    src/program.cpp
    src/loader.cpp
    src/playlist.cpp)
set(SHADERTOY_PUBLIC_HEADERS
    include/shadertoy/inputs.hpp
    include/shadertoy/program.hpp
    include/shadertoy/loader.hpp
    include/shadertoy/playlist.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/include/shadertoy/config.hpp)

if(HAVE_GL)
  # gl_renderer pulls in stb_image (vendored) for texture channels.
  list(APPEND SHADERTOY_SOURCES src/gl_renderer.cpp src/stb_image_impl.cpp)
  list(APPEND SHADERTOY_PUBLIC_HEADERS include/shadertoy/gl_renderer.hpp)
endif()
if(HAVE_VULKAN)
  list(APPEND SHADERTOY_SOURCES src/spirv_compile.cpp src/vk_renderer.cpp)
  list(APPEND SHADERTOY_PUBLIC_HEADERS
       include/shadertoy/spirv_compile.hpp
       include/shadertoy/vk_renderer.hpp)
endif()

# No explicit SHARED: honor BUILD_SHARED_LIBS so the library can be built either
# way (default static). POSITION_INDEPENDENT_CODE keeps a static build linkable
# into a PIE executable.
add_library(shadertoy ${SHADERTOY_SOURCES})
add_library(shadertoy-cxx::shadertoy ALIAS shadertoy)

set_target_properties(shadertoy PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET default)

target_include_directories(shadertoy PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Vendored header-only third-party deps (not exposed in public headers).
target_include_directories(shadertoy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(HAVE_GL)
  target_link_libraries(shadertoy PUBLIC PkgConfig::GLESV2)
endif()
if(HAVE_VULKAN)
  target_link_libraries(shadertoy PUBLIC Vulkan::Vulkan)
endif()

message(STATUS "shadertoy-cxx: OpenGL ES 3 back-end: ${HAVE_GL}")
message(STATUS "shadertoy-cxx: Vulkan back-end:      ${HAVE_VULKAN}")

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS shadertoy
    EXPORT shadertoy-cxx-targets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES ${SHADERTOY_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/shadertoy)

# Bundled shader set — only license-compatible shaders (see shaders/CREDITS.md).
install(FILES
    shaders/default.frag
    shaders/trail.json
    shaders/X3yXRd.json
    shaders/CREDITS.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/shadertoy-cxx/shaders)

# CMake package config.
set(SHADERTOY_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/shadertoy-cxx)
install(EXPORT shadertoy-cxx-targets
    FILE shadertoy-cxx-targets.cmake
    NAMESPACE shadertoy-cxx::
    DESTINATION ${SHADERTOY_INSTALL_CMAKEDIR})

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shadertoy-cxx-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/shadertoy-cxx-config.cmake
    INSTALL_DESTINATION ${SHADERTOY_INSTALL_CMAKEDIR})
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/shadertoy-cxx-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/shadertoy-cxx-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/shadertoy-cxx-config-version.cmake
    DESTINATION ${SHADERTOY_INSTALL_CMAKEDIR})

# pkg-config (so non-CMake consumers can also use a CMake-built install).
set(SHADERTOY_PC_REQUIRES "")
if(HAVE_GL)
  string(APPEND SHADERTOY_PC_REQUIRES "glesv2 ")
endif()
if(HAVE_VULKAN)
  string(APPEND SHADERTOY_PC_REQUIRES "vulkan ")
endif()
string(STRIP "${SHADERTOY_PC_REQUIRES}" SHADERTOY_PC_REQUIRES)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shadertoy-cxx.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/shadertoy-cxx.pc
    @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/shadertoy-cxx.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(SHADERTOY_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
