# broportal_test(<name> [TIMEOUT s]): one real ctest from <name>.cpp. Exit
# code 77 is a skip (a tool or service genuinely absent, reason printed),
# anything else nonzero a failure. The Linux tests each start their own
# dbus-daemon: nothing touches the user's session bus.
function(broportal_test name)
    cmake_parse_arguments(T "" "TIMEOUT" "" ${ARGN})
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE broportal::broportal)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
    broportal_warnings(${name})
    if(NOT T_TIMEOUT)
        set(T_TIMEOUT 60)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT} SKIP_RETURN_CODE 77)
endfunction()

# Portable: every platform.
broportal_test(test_types)

if(BROPORTAL_LINUX)
    broportal_test(test_dbus_helpers)
    broportal_test(test_request_session)
    broportal_test(test_filechooser)
    broportal_test(test_screenshot)
    broportal_test(test_screencast)
    broportal_test(test_remotedesktop)
    broportal_test(test_settings)
    broportal_test(test_inhibit)
    broportal_test(test_openuri)
    broportal_test(test_globalshortcuts)
    broportal_test(test_full_backend)
    broportal_test(test_concurrency)
    # The real xdg-desktop-portal frontend in front of the backend.
    broportal_test(test_xdp_frontend TIMEOUT 120)
else()
    broportal_test(test_unavailable)
endif()

if(BROPORTAL_ENABLE_API)
    add_executable(broportal_test_api
        test_portal_api.cpp
    )
    target_include_directories(broportal_test_api PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/api
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(broportal_test_api PRIVATE
        broportal_api
        broportal
        bronze_runtime_shared
    )
    broportal_warnings(broportal_test_api)
    if(WIN32)
        target_compile_definitions(broportal_test_api PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        add_custom_command(TARGET broportal_test_api POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broportal_test_api>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broportal_test_api>
        )
    endif()

    if(NOT BRASS_HOST_BACKEND)
        message(STATUS "broportal: broportal_test_api not registered (brass has no code generator for this target)")
    else()
        add_test(NAME broportal_test_api COMMAND broportal_test_api)
        set_tests_properties(broportal_test_api PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

        # A collection at every allocation over a Debug bronze (-O0, and
        # instrumented in the coverage job) takes minutes, not seconds. The
        # limit is there to catch a hang, not to time the run.
        add_test(NAME broportal_test_api_gc_stress COMMAND broportal_test_api)
        set_tests_properties(broportal_test_api_gc_stress PROPERTIES
            TIMEOUT 600
            SKIP_RETURN_CODE 77
            ENVIRONMENT "BRONZE_GC_STRESS=1;BRONZE_GC_POISON=1"
        )
        if(WIN32)
            set_tests_properties(broportal_test_api PROPERTIES
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:broportal_test_api>;$ENV{PATH}")
            set_property(TEST broportal_test_api_gc_stress APPEND PROPERTY
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:broportal_test_api>;$ENV{PATH}")
        endif()
    endif()
endif()
