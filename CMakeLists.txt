cmake_minimum_required(VERSION 3.24)

project(broportal VERSION 0.1.0 LANGUAGES CXX)

# Dependencies (cmake/bro_deps.cmake): an outer project's target wins, else the
# ../<name> working tree beside the top-level project, else the pinned commit,
# fetched at configure.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bro_deps.cmake)

# Targets
#   broportal::broportal   an xdg-desktop-portal backend (org.freedesktop.impl.portal.*)
#
# Sources:
#   src/types.cpp, src/availability.cpp
#                          portable, built everywhere
#   everything else        Linux: sd-bus (libsystemd); the public headers name
#                          sd-bus types, so libsystemd is a PUBLIC dependency
# Off Linux the library is the value types plus broportal::available(),
# which says why there is no backend.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_broportal_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_broportal_top_level ON)
endif()

option(BROPORTAL_BUILD_TESTS "Build the broportal tests" ${_broportal_top_level})

# Coverage. Scoped to a top-level build: a consumer that add_subdirectory()s
# broportal has no business having its own build instrumented from in here.
option(BROPORTAL_COVERAGE "Instrument the build for gcov coverage (GCC/Clang)" OFF)
if(BROPORTAL_COVERAGE AND _broportal_top_level)
    if(MSVC)
        message(FATAL_ERROR "BROPORTAL_COVERAGE requires GCC or Clang (gcov instrumentation)")
    endif()
    add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
    add_link_options(--coverage)
    message(STATUS "broportal: coverage instrumentation ON (-O0)")
endif()

# MSVC runtime: the default, the DLL CRT (/MD, /MDd), as bro builds with. The
# api's tests load bronze_runtime_shared.dll and pass std::string and vectors
# across that boundary; with the static CRT each module has its own heap, and
# memory freed on the other side is a Debug assert (__acrt_first_block ==
# header) and silent heap corruption in Release.
if(MSVC AND _broportal_top_level)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

function(broportal_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /MP)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

find_package(Threads REQUIRED)

set(BROPORTAL_SOURCES
    src/types.cpp
    src/availability.cpp)

set(BROPORTAL_LINUX OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BROPORTAL_LINUX ON)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

    bro_dependency(brodbus GITHUB wlejon/brodbus OPTIONS BRODBUS_BUILD_TESTS=OFF)

    list(APPEND BROPORTAL_SOURCES
        src/backend.cpp
        src/dbus_helpers.cpp
        src/filechooser.cpp
        src/globalshortcuts.cpp
        src/inhibit.cpp
        src/openuri.cpp
        src/remotedesktop.cpp
        src/request.cpp
        src/screencast.cpp
        src/screenshot.cpp
        src/session.cpp
        src/settings.cpp)
else()
    message(STATUS "broportal: not Linux; building the value types and broportal::available() only")
endif()

add_library(broportal STATIC ${BROPORTAL_SOURCES})
add_library(broportal::broportal ALIAS broportal)

target_include_directories(broportal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

broportal_warnings(broportal)

target_link_libraries(broportal PUBLIC Threads::Threads)
if(BROPORTAL_LINUX)
    target_link_libraries(broportal PUBLIC PkgConfig::SYSTEMD brodbus::brodbus)
endif()

# --- Standalone Bronze JavaScript API ---
# On by default only when broportal is the top-level project. A library that
# consumes broportal then never needs bronze checked out; bro, which does want
# the binding, sets BROPORTAL_ENABLE_API ON itself.
option(BROPORTAL_ENABLE_API "Build Bronze JavaScript API" ${PROJECT_IS_TOP_LEVEL})
if(BROPORTAL_ENABLE_API)
    # bronze (with brass, its code generator) through cmake/bro_deps.cmake: an
    # outer project's target, else ../bronze beside the top-level project, else
    # the pinned commit, fetched at configure.
    bro_dependency(bronze GITHUB wlejon/bronze TARGET bronze_runtime_shared
                   OPTIONS BRONZE_BUILD_SHARED_RUNTIME=ON BRONZE_BUILD_TESTS=OFF)

    add_subdirectory(src/api)
endif()

if(BROPORTAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
