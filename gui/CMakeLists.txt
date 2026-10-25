# elm327gui, the graphical front-end. The worker thread that talks to the adapter, and the scales of
# the gauges of widget mode, are the library elm327gui_core, which does not depend on ImGui, so the
# tests run them without a display. The window itself uses Dear ImGui (the git submodule
# third_party/imgui) on GLFW and OpenGL 3.
if(ELM327_BUILD_TESTS OR NOT ELM327_BUILD_GUI STREQUAL "OFF")
    add_library(elm327gui_core STATIC gauge.cpp worker.cpp)
    # The sources include each other as "gui/<file>.hpp".
    target_include_directories(elm327gui_core PUBLIC "${PROJECT_SOURCE_DIR}")
    target_link_libraries(elm327gui_core
        PUBLIC elm327::elm327
        PRIVATE common::options)
endif()

if(ELM327_BUILD_GUI STREQUAL "OFF")
    return()
endif()

set(ELM327_IMGUI_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")
set(ELM327_GUI_MISSING "")
if(NOT EXISTS "${ELM327_IMGUI_DIR}/imgui.cpp")
    list(APPEND ELM327_GUI_MISSING "the imgui submodule (run 'git submodule update --init')")
endif()
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
    list(APPEND ELM327_GUI_MISSING "GLFW 3.3 or later (sudo apt install libglfw3-dev)")
endif()
if(ELM327_GUI_MISSING)
    list(JOIN ELM327_GUI_MISSING " and " ELM327_GUI_MISSING_TEXT)
    if(ELM327_BUILD_GUI STREQUAL "ON")
        message(FATAL_ERROR "elm327gui needs ${ELM327_GUI_MISSING_TEXT}")
    endif()
    message(STATUS "elm327gui is not built: it needs ${ELM327_GUI_MISSING_TEXT}")
    return()
endif()

# Dear ImGui has no build of its own. It is compiled as it comes, without this project's warnings
# and checks, and its headers are system headers for the code that includes them. The OpenGL 3
# backend loads OpenGL at run time, so no OpenGL development files are needed.
add_library(imgui STATIC
    ${ELM327_IMGUI_DIR}/imgui.cpp
    ${ELM327_IMGUI_DIR}/imgui_draw.cpp
    ${ELM327_IMGUI_DIR}/imgui_tables.cpp
    ${ELM327_IMGUI_DIR}/imgui_widgets.cpp
    ${ELM327_IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${ELM327_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui SYSTEM PUBLIC "${ELM327_IMGUI_DIR}" "${ELM327_IMGUI_DIR}/backends")
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS GLFW_INCLUDE_NONE)
# The code generation of the project: no exceptions, no RTTI.
target_compile_options(imgui PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(imgui PUBLIC glfw ${CMAKE_DL_LIBS})
set_target_properties(imgui PROPERTIES CXX_CLANG_TIDY "")

add_executable(elm327gui main.cpp app.cpp dashboard.cpp)
target_link_libraries(elm327gui PRIVATE elm327gui_core imgui common::options)
install(TARGETS elm327gui RUNTIME DESTINATION bin)
