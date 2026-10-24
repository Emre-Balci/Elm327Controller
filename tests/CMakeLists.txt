# Test sources mirror the header groups in include/elm327/<group>/, and cli/ tests the command-line
# tool. Like Common's tests, they use the heap, std::string and std::thread, and are outside the
# compliance scope.
add_executable(elm327_tests
    test_main.cpp
    adapter/test_adapter.cpp
    adapter/test_response.cpp
    cli/test_commands.cpp
    cli/test_options.cpp
    obd/test_client.cpp
    obd/test_dtc.cpp
    obd/test_message.cpp
    obd/test_pid.cpp
    obd/test_vin.cpp)

# test_framework.hpp and test_support.hpp live here; tests in group subdirectories include them by name.
target_include_directories(elm327_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(elm327_tests PRIVATE elm327ctl_core elm327_sim common::options)

add_test(NAME elm327_tests COMMAND elm327_tests)
