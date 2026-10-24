cmake_minimum_required(VERSION 3.21)

project(elm327controller
    VERSION 0.1.0
    DESCRIPTION "Command-line controller for ELM327 OBD-II adapters"
    LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Elm327Controller targets Linux only (use WSL on Windows)")
endif()

option(ELM327_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(ELM327_BUILD_SIMULATOR "Build the elm327sim adapter simulator" ${PROJECT_IS_TOP_LEVEL})
option(ELM327_WARNINGS_AS_ERRORS "Treat warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(ELM327_ENABLE_CLANG_TIDY "Run clang-tidy on this project's sources during the build" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Common, the real-time foundation library, is the git submodule third_party/common. Every target
# here links its compiler policy common::options (warnings, -ffp-contract=off, -fno-exceptions,
# -fno-rtti); the cache variable COMMON_SANITIZER selects sanitizers for Common and this project.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/common/CMakeLists.txt")
    message(FATAL_ERROR "third_party/common is empty: run 'git submodule update --init'")
endif()
set(COMMON_WARNINGS_AS_ERRORS ${ELM327_WARNINGS_AS_ERRORS})
add_subdirectory(third_party/common)

# Set after Common is added, so that only this project's targets are checked.
if(ELM327_ENABLE_CLANG_TIDY)
    find_program(ELM327_CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY ${ELM327_CLANG_TIDY_EXE})
endif()

configure_file(cmake/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/elm327/version.hpp" @ONLY)

# The library: the ELM327 adapter protocol and the OBD-II services. Sources mirror the header
# groups in include/elm327/<group>/.
add_library(elm327
    src/adapter/adapter.cpp
    src/adapter/response.cpp
    src/obd/client.cpp
    src/obd/dtc.cpp
    src/obd/message.cpp
    src/obd/pid.cpp
    src/obd/vin.cpp)
add_library(elm327::elm327 ALIAS elm327)

target_include_directories(elm327 PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(elm327
    PUBLIC common::common
    PRIVATE common::options)

# The command-line tool. Everything except main() is a library, so the tests can run commands
# in-process.
add_library(elm327ctl_core STATIC
    cli/commands.cpp
    cli/console.cpp
    cli/options.cpp
    cli/signals.cpp)
target_include_directories(elm327ctl_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(elm327ctl_core
    PUBLIC elm327::elm327
    PRIVATE common::options)

add_executable(elm327ctl cli/main.cpp)
target_link_libraries(elm327ctl PRIVATE elm327ctl_core common::options)

install(TARGETS elm327ctl RUNTIME DESTINATION bin)

# The simulated adapter: a fixture of the tests, and the elm327sim tool to try the CLI without
# hardware.
if(ELM327_BUILD_TESTS OR ELM327_BUILD_SIMULATOR)
    add_subdirectory(sim)
endif()

if(ELM327_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
