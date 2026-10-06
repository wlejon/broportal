add_library(broportal_api STATIC
    api.cpp
    host_class.cpp
    native_filechooser.cpp
    native_screenshot.cpp
    native_screencast.cpp
    native_openuri.cpp
)

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
