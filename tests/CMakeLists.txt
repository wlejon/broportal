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
    # The real xdg-desktop-portal frontend in front of the backend.
    broportal_test(test_xdp_frontend TIMEOUT 120)
else()
    broportal_test(test_unavailable)
endif()
