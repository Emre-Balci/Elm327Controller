cmake_minimum_required(VERSION 3.21)

project(elm327controller
    VERSION 0.3.0
    DESCRIPTION "Library, interactive command-line tool and GUI for ELM327 OBD-II adapters"
    LANGUAGES CXX)

if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows"))
    message(FATAL_ERROR "Elm327Controller targets Linux and Windows only")
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

if(MSVC)
    # CMake's default flags turn on exceptions and RTTI (/EHsc /GR), which common::options turns off
    # for every target here; removing the defaults spares cl's warning D9025 about overriding them.
    string(REGEX REPLACE "(^| )/(EHsc|GR)( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    # The programs take their arguments, e.g. file paths, in UTF-8 as Common expects them (the
    # manifest makes UTF-8 the code page of the process, from Windows 10 1903 on).
    set(ELM327_WINDOWS_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/utf8.manifest")
endif()
if(WIN32)
    # <windows.h> without its min and max macros, which break std::min and numeric_limits::max, and
    # without the parts of the API that nothing here uses.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Common, the real-time foundation library, is the git submodule third_party/common. Every target
# here links its compiler policy common::options (warnings, -ffp-contract=off, -fno-exceptions,
# -fno-rtti, or their MSVC counterparts); the cache variable COMMON_SANITIZER selects sanitizers for
# Common and this project.
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
