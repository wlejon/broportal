cmake_minimum_required(VERSION 3.24)

project(broportal VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_broportal_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_broportal_top_level ON)
endif()

option(BROPORTAL_BUILD_TESTS "Build broportal tests" ${_broportal_top_level})

function(broportal_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /MP)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

file(GLOB_RECURSE BROPORTAL_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
)

add_library(broportal STATIC ${BROPORTAL_SOURCES})
add_library(broportal::broportal ALIAS broportal)

target_include_directories(broportal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

broportal_warnings(broportal)

target_link_libraries(broportal
    PUBLIC
        Threads::Threads
        PkgConfig::SYSTEMD
        PkgConfig::PIPEWIRE
)

if(BROPORTAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
