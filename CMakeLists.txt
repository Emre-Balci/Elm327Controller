cmake_minimum_required(VERSION 3.21)

project(elm327controller
    VERSION 0.2.0
    DESCRIPTION "Library, interactive command-line tool and GUI for ELM327 OBD-II adapters"
    LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Elm327Controller targets Linux only (use WSL on Windows)")
endif()

option(ELM327_BUILD_CLI "Build the elm327ctl command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(ELM327_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(ELM327_BUILD_SIMULATOR "Build the elm327sim adapter simulator" ${PROJECT_IS_TOP_LEVEL})
option(ELM327_WARNINGS_AS_ERRORS "Treat warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(ELM327_ENABLE_CLANG_TIDY "Run clang-tidy on this project's sources during the build" OFF)
# AUTO builds the GUI when GLFW and the imgui submodule are there, ON requires them.
if(PROJECT_IS_TOP_LEVEL)
    set(ELM327_GUI_DEFAULT AUTO)
else()
    set(ELM327_GUI_DEFAULT OFF)
endif()
set(ELM327_BUILD_GUI ${ELM327_GUI_DEFAULT} CACHE STRING "Build the elm327gui ImGui front-end: ON, OFF or AUTO")
set_property(CACHE ELM327_BUILD_GUI PROPERTY STRINGS ON OFF AUTO)

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

# The two parts: the static library libelm327, and the front-ends built on it. The tests run the
# command-line tool and the GUI's worker in-process, so their libraries are built for the tests too.
add_subdirectory(lib)

if(ELM327_BUILD_CLI OR ELM327_BUILD_TESTS)
    add_subdirectory(cli)
endif()

add_subdirectory(gui)

# The simulated adapter: a fixture of the tests, and the elm327sim tool to try the front-ends
# without hardware.
if(ELM327_BUILD_TESTS OR ELM327_BUILD_SIMULATOR)
    add_subdirectory(sim)
endif()

if(ELM327_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
