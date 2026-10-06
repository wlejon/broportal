add_library(broportal_api STATIC
    api.cpp
    host_class.cpp
)

# The handler bridges queue requests from the D-Bus backend, which exists only
# on Linux. Elsewhere bro.portal is { available: false, reason } and nothing
# else (api.cpp), so they are not built there.
if(BROPORTAL_LINUX)
    target_sources(broportal_api PRIVATE
        native_filechooser.cpp
        native_screenshot.cpp
        native_screencast.cpp
        native_openuri.cpp)
endif()

target_include_directories(broportal_api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
)

target_link_libraries(broportal_api PUBLIC
    broportal
    bronze_runtime_shared
)

target_compile_features(broportal_api PUBLIC cxx_std_20)

broportal_warnings(broportal_api)
