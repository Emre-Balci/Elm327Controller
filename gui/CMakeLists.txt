# elm327gui, the graphical front-end. The worker thread that talks to the adapter, the scales of the
# gauges of widget mode, and the files of the Features tab are the library elm327gui_core, which does
# not depend on ImGui, so the tests run them without a display. The window itself uses Dear ImGui
# (the git submodule third_party/imgui) on GLFW and OpenGL 3; on Android it is the shared library of
# the app's native activity (android/), on the NDK's glue and OpenGL ES 3; on iOS an app bundle, on
# UIKit and Metal.
if(ELM327_BUILD_TESTS OR NOT ELM327_BUILD_GUI STREQUAL "OFF")
    add_library(elm327gui_core STATIC features.cpp gauge.cpp worker.cpp)
    # The sources include each other as "gui/<file>.hpp".
    target_include_directories(elm327gui_core PUBLIC "${PROJECT_SOURCE_DIR}")
    target_link_libraries(elm327gui_core
        PUBLIC elm327::elm327
        PRIVATE common::options)
    if(WIN32)
        # The configuration directory is a known folder of the shell (shell32, with its GUID in uuid),
        # which allocates the path with the COM allocator (ole32).
        target_link_libraries(elm327gui_core PRIVATE shell32 ole32 uuid)
    endif()
endif()

if(ELM327_BUILD_GUI STREQUAL "OFF")
    return()
endif()

set(ELM327_IMGUI_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")
set(ELM327_GUI_MISSING "")
if(NOT (ANDROID OR IOS))
    find_package(glfw3 3.3 QUIET)
endif()
if(NOT EXISTS "${ELM327_IMGUI_DIR}/imgui.cpp")
    list(APPEND ELM327_GUI_MISSING "the imgui submodule (run 'git submodule update --init')")
endif()
if(ANDROID OR IOS)
    # The NDK, or Xcode, brings everything the app needs.
elseif(NOT glfw3_FOUND)
    if(WIN32)
        list(APPEND ELM327_GUI_MISSING
            "GLFW 3.3 or later (vcpkg install glfw3, or CMAKE_PREFIX_PATH set to an installed GLFW)")
    else()
        list(APPEND ELM327_GUI_MISSING "GLFW 3.3 or later (sudo apt install libglfw3-dev)")
    endif()
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
# backend loads OpenGL at run time, so no OpenGL development files are needed. On Android the
# backends are those of the native activity and of OpenGL ES 3, and the NDK's glue, which is C,
# comes along. On iOS it is the Metal backend (Objective-C++, with ARC); the app handles the input.
add_library(imgui STATIC
    ${ELM327_IMGUI_DIR}/imgui.cpp
    ${ELM327_IMGUI_DIR}/imgui_draw.cpp
    ${ELM327_IMGUI_DIR}/imgui_tables.cpp
    ${ELM327_IMGUI_DIR}/imgui_widgets.cpp
    ${ELM327_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui SYSTEM PUBLIC "${ELM327_IMGUI_DIR}" "${ELM327_IMGUI_DIR}/backends")
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
if(ANDROID)
    set(ELM327_ANDROID_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
    target_sources(imgui PRIVATE
        ${ELM327_IMGUI_DIR}/backends/imgui_impl_android.cpp
        ${ELM327_ANDROID_GLUE_DIR}/android_native_app_glue.c)
    target_include_directories(imgui SYSTEM PUBLIC "${ELM327_ANDROID_GLUE_DIR}")
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
    target_link_libraries(imgui PUBLIC android EGL GLESv3 log)
elseif(IOS)
    target_sources(imgui PRIVATE ${ELM327_IMGUI_DIR}/backends/imgui_impl_metal.mm)
    target_compile_options(imgui PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(imgui PUBLIC "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
else()
    target_sources(imgui PRIVATE ${ELM327_IMGUI_DIR}/backends/imgui_impl_glfw.cpp)
    target_compile_definitions(imgui PUBLIC GLFW_INCLUDE_NONE)
    target_link_libraries(imgui PUBLIC glfw ${CMAKE_DL_LIBS})
endif()
# The code generation of the project: no exceptions, no RTTI (and with MSVC, a standard library
# without exceptions, and the sources read as the UTF-8 they are).
if(MSVC)
    target_compile_options(imgui PRIVATE /EHs-c- /GR- /utf-8)
    target_compile_definitions(imgui PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(imgui PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
endif()
set_target_properties(imgui PROPERTIES CXX_CLANG_TIDY "")

if(ANDROID)
    # The library that MainActivity, a NativeActivity, loads: android_main() is its entry point,
    # through ANativeActivity_onCreate() of the glue, which the linker must keep. The simulator is
    # in it as the demo device.
    add_library(elm327gui SHARED main_android.cpp android_bridge.cpp app.cpp dashboard.cpp)
    target_link_libraries(elm327gui PRIVATE elm327gui_core elm327_sim imgui common::options)
    target_link_options(elm327gui PRIVATE "-Wl,-u,ANativeActivity_onCreate")
    return()
endif()

if(IOS)
    # The app bundle: UIKit and Metal (main_ios.mm) and Core Bluetooth (ios_bridge.mm), Objective-C++
    # with ARC, around the same App; the simulator is in it as the demo device. Xcode signs it with
    # the team of CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM.
    add_executable(elm327gui MACOSX_BUNDLE main_ios.mm ios_bridge.mm app.cpp dashboard.cpp ios/Assets.xcassets)
    set_source_files_properties(ios/Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # The Objective-C++ was written without a Mac to compile it: until it has been built warning-free
    # with Xcode, its warnings are shown but do not fail the build.
    set_source_files_properties(main_ios.mm ios_bridge.mm PROPERTIES COMPILE_OPTIONS "-Wno-error")
    set_target_properties(elm327gui PROPERTIES
        OUTPUT_NAME Elm327Controller
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.emrebalci.elm327controller
        MACOSX_BUNDLE_BUNDLE_NAME Elm327Controller
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER io.github.emrebalci.elm327controller
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic)
    target_link_libraries(elm327gui PRIVATE elm327gui_core elm327_sim imgui common::options
        "-framework UIKit" "-framework CoreBluetooth")
    return()
endif()

add_executable(elm327gui main.cpp app.cpp dashboard.cpp ${ELM327_WINDOWS_MANIFEST})
target_link_libraries(elm327gui PRIVATE elm327gui_core imgui common::options)
install(TARGETS elm327gui RUNTIME DESTINATION bin)

get_target_property(ELM327_GLFW_TYPE glfw TYPE)
if(WIN32 AND (ELM327_GLFW_TYPE STREQUAL "SHARED_LIBRARY"))
    # Windows finds a DLL next to the executable (there is no rpath).
    add_custom_command(TARGET elm327gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:glfw> $<TARGET_FILE_DIR:elm327gui>)
    install(FILES $<TARGET_FILE:glfw> DESTINATION bin)
endif()
