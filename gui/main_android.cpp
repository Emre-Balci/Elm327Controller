/// @file main_android.cpp
/// @brief elm327gui on Android: the app's native activity, drawn with Dear ImGui on OpenGL ES 3.
///
/// MainActivity (android/app/src/main/java/.../MainActivity.java) is a NativeActivity. The NDK's glue
/// (android_native_app_glue) runs android_main() on a thread of its own, which draws the window as
/// main.cpp does on the desktop, with the same App. The Worker talks to the adapter on its thread,
/// reaches Bluetooth adapters through MainActivity (see android_bridge.hpp), and wakes the looper of
/// android_main() (ALooper_wake()) when it has something new.
///
/// The app draws while it is visible; in the background it has no window, but the worker keeps the
/// connection. While connected, the screen stays on. The window keeps clear of the status bar, the
/// navigation bar and a display cutout, which MainActivity reports as insets.

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/configuration.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"
#include "gui/android_bridge.hpp"
#include "gui/app.hpp"
#include "gui/features.hpp"
#include "gui/worker.hpp"
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"  // ImGuiViewportP: the work area, which keeps clear of the insets.

namespace {

using elm327::gui::App;
using elm327::gui::Worker;
namespace bridge = elm327::gui::bridge;

/// The tag of the app's messages in the system log (logcat).
constexpr const char* kLogTag{"elm327gui"};

/// Roboto, the font of Android itself, in place of ImGui's built-in one.
constexpr const char* kFontPath{"/system/fonts/Roboto-Regular.ttf"};
/// In density-independent pixels: a little larger than on the desktop, for fingers.
constexpr float kFontSize{16.0F};

/// The screen density of a scale of 1 (mdpi), ACONFIGURATION_DENSITY_MEDIUM, and the largest real
/// one (ACONFIGURATION_DENSITY_ANY and _NONE lie above it).
constexpr std::int32_t kBaseDensity{160};
constexpr std::int32_t kMaxDensity{0xFFF0};

/// What ImGui's sizes grow by for touch: the area around a widget that a finger may hit, and the
/// width of the scroll bars, in density-independent pixels.
constexpr float kTouchPadding{4.0F};
constexpr float kScrollbarSize{20.0F};

/// How long the looper waits for an event before the next frame, in ms: while the worker is busy or
/// monitoring, and otherwise. A result of the worker or an input event ends the wait at once.
constexpr int kActiveWait{16};
constexpr int kIdleWait{250};

/// Frames drawn at once after an event: ImGui takes one input event per frame, and a tap is a
/// press and a release.
constexpr std::uint32_t kFramesAfterEvent{4U};

/// Events handled before the next frame, and characters taken from the keyboard per frame.
constexpr std::uint32_t kMaxEvents{64U};
constexpr std::uint32_t kMaxCharacters{64U};

/// Frames at most: a bound for the loop, years at any frame rate.
constexpr std::uint64_t kMaxFrames{std::numeric_limits<std::uint64_t>::max()};

/// The flag of a window that keeps the screen on.
constexpr std::uint32_t kKeepScreenOn{static_cast<std::uint32_t>(AWINDOW_FLAG_KEEP_SCREEN_ON)};

/// The parts of a name that make a paired device the adapter to offer first.
constexpr std::array<std::string_view, 6U> kAdapterNames{{"OBD", "obd", "Obd", "ELM", "elm", "Elm"}};

/// The display of the window: EGL's connection, the surface on the window and the OpenGL ES context.
struct Display final {
    EGLDisplay display{EGL_NO_DISPLAY};
    EGLSurface surface{EGL_NO_SURFACE};
    EGLContext context{EGL_NO_CONTEXT};
};

/// What android_main() keeps between the commands of the glue.
struct Session final {
    android_app* glue{nullptr};
    App* app{nullptr};
    Worker* worker{nullptr};
    Display display{};
    bool imgui{false};      ///< ImGui's context exists.
    bool drawing{false};    ///< There is a window, with its display and the backends of ImGui.
    bool keyboard{false};   ///< The on-screen keyboard was asked for.
    bool screen_on{false};  ///< The window keeps the screen on.
    std::uint32_t frames_left{0U};
};

void log_error(const char* const text) noexcept {
    static_cast<void>(__android_log_write(ANDROID_LOG_ERROR, kLogTag, text));
}

/// Wakes up the looper of android_main(): called by the worker's thread when it has published
/// something.
void wake_up(void* const looper) noexcept {
    ALooper_wake(static_cast<ALooper*>(looper));
}

/// @return true if the file @p path can be opened for reading.
bool readable(const char* const path) noexcept {
    common::File file{};
    const bool opened{is_ok(file.open(path, common::FileOptions{}))};  // Read-only.
    static_cast<void>(file.close());
    return opened;
}

/// @return true if @p name contains @p part.
bool contains(const std::string_view name, const std::string_view part) noexcept {
    return name.find(part) != std::string_view::npos;
}

/// The device to offer at the start: the first paired device whose name looks like an OBD adapter,
/// otherwise the simulator. The first look asks for the permission for nearby devices.
void choose_device(elm327::ConnectionSettings& settings) noexcept {
    std::array<bridge::PairedDevice, 16U> paired{};
    std::size_t count{0U};
    static_cast<void>(bridge::paired_devices(paired, count));
    bool chosen{false};
    for (std::size_t index{0U}; (!chosen) && (index < count); ++index) {
        const std::string_view name{paired[index].name.view()};
        for (const std::string_view part : kAdapterNames) {
            chosen = chosen || contains(name, part);
        }
        if (chosen) {
            settings.device.clear();
            static_cast<void>(settings.device.append(elm327::kBluetoothPrefix) &&
                              settings.device.append(paired[index].address.view()));
        }
    }
    if (!chosen) {
        static_cast<void>(settings.device.assign(bridge::kSimulatorDevice));
    }
}

void close_display(Display& display) noexcept {
    if (display.display != EGL_NO_DISPLAY) {
        static_cast<void>(eglMakeCurrent(display.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        if (display.context != EGL_NO_CONTEXT) {
            static_cast<void>(eglDestroyContext(display.display, display.context));
        }
        if (display.surface != EGL_NO_SURFACE) {
            static_cast<void>(eglDestroySurface(display.display, display.surface));
        }
        static_cast<void>(eglTerminate(display.display));
    }
    display = Display{};
}

/// Opens an OpenGL ES 3 display on @p window. @return false if that fails (nothing is open then).
bool open_display(ANativeWindow* const window, Display& display) noexcept {
    display.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    bool ok{(display.display != EGL_NO_DISPLAY) && (eglInitialize(display.display, nullptr, nullptr) == EGL_TRUE)};
    EGLConfig config{nullptr};
    if (ok) {
        const std::array<EGLint, 11U> attributes{{EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,
                                                  EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE,
                                                  8, EGL_NONE}};
        EGLint count{0};
        ok = (eglChooseConfig(display.display, attributes.data(), &config, 1, &count) == EGL_TRUE) && (count > 0);
    }
    if (ok) {
        EGLint format{0};
        static_cast<void>(eglGetConfigAttrib(display.display, config, EGL_NATIVE_VISUAL_ID, &format));
        static_cast<void>(ANativeWindow_setBuffersGeometry(window, 0, 0, format));
        const std::array<EGLint, 3U> context_attributes{{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE}};
        display.context = eglCreateContext(display.display, config, EGL_NO_CONTEXT, context_attributes.data());
        display.surface = eglCreateWindowSurface(display.display, config, window, nullptr);
        ok = (display.context != EGL_NO_CONTEXT) && (display.surface != EGL_NO_SURFACE) &&
             (eglMakeCurrent(display.display, display.surface, display.surface, display.context) == EGL_TRUE);
    }
    if (!ok) {
        close_display(display);
    }
    return ok;
}

/// The scale of the screen: its density relative to mdpi, e.g. 3 for xxhdpi.
float screen_scale(AConfiguration* const configuration) noexcept {
    const std::int32_t density{AConfiguration_getDensity(configuration)};
    return ((density > 0) && (density < kMaxDensity)) ? (static_cast<float>(density) / static_cast<float>(kBaseDensity))
                                                      : 1.0F;
}

/// Creates ImGui's context, with the style scaled for the screen and for touch.
void create_imgui(Session& session) noexcept {
    const float scale{screen_scale(session.glue->config)};
    IMGUI_CHECKVERSION();
    static_cast<void>(ImGui::CreateContext());
    ImGuiIO& io{ImGui::GetIO()};
    io.IniFilename = nullptr;  // Nothing to remember between runs.
    ImGui::StyleColorsDark();
    ImGuiStyle& style{ImGui::GetStyle()};
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FrameRounding = 3.0F * scale;
    style.TouchExtraPadding = ImVec2{kTouchPadding * scale, kTouchPadding * scale};
    style.ScrollbarSize = kScrollbarSize * scale;
    if (readable(kFontPath)) {
        static_cast<void>(io.Fonts->AddFontFromFileTTF(kFontPath, kFontSize));
    }
    session.imgui = true;
}

/// A window appeared: opens its display and starts ImGui's backends on it.
void start_drawing(Session& session) noexcept {
    ANativeWindow* const window{session.glue->window};
    if ((!session.drawing) && (window != nullptr)) {
        if (open_display(window, session.display)) {
            if (!session.imgui) {
                create_imgui(session);
            }
            static_cast<void>(ImGui_ImplAndroid_Init(window));
            static_cast<void>(ImGui_ImplOpenGL3_Init("#version 300 es"));
            session.drawing = true;
            session.frames_left = kFramesAfterEvent;
        } else {
            log_error("cannot open an OpenGL ES 3 display");
        }
    }
}

/// The window goes away, e.g. as the app goes into the background.
void stop_drawing(Session& session) noexcept {
    if (session.drawing) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        close_display(session.display);
        session.drawing = false;
        session.keyboard = false;
    }
}

void handle_command(android_app* const glue, const std::int32_t command) noexcept {
    Session& session{*static_cast<Session*>(glue->userData)};
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            start_drawing(session);
            break;
        case APP_CMD_TERM_WINDOW:
            stop_drawing(session);
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_CONTENT_RECT_CHANGED:
        case APP_CMD_GAINED_FOCUS:
            session.frames_left = kFramesAfterEvent;
            break;
        default:
            break;
    }
}

std::int32_t handle_input(android_app* const glue, AInputEvent* const event) noexcept {
    Session& session{*static_cast<Session*>(glue->userData)};
    std::int32_t handled{0};
    if (session.drawing) {
        session.frames_left = kFramesAfterEvent;
        handled = ImGui_ImplAndroid_HandleInputEvent(event);
    }
    return handled;
}

/// Handles the events of the glue: waits at most @p timeout ms for the first (-1: until one comes),
/// then takes the others that are there.
void handle_events(android_app* const glue, const int timeout) noexcept {
    int wait{timeout};
    bool more{true};
    for (std::uint32_t round{0U}; more && (round < kMaxEvents) && (glue->destroyRequested == 0); ++round) {
        void* data{nullptr};
        const int result{ALooper_pollOnce(wait, nullptr, nullptr, &data)};
        android_poll_source* const source{static_cast<android_poll_source*>(data)};
        if ((result >= 0) && (source != nullptr)) {
            source->process(glue, source);
        }
        // An event or a callback: look for more without waiting. A wake-up or the timeout: draw.
        more = (result >= 0) || (result == ALOOPER_POLL_CALLBACK);
        wait = 0;
    }
}

/// The window keeps clear of the system bars and a display cutout: from the next frame on, the
/// main viewport's work area, where App draws, leaves the insets out.
void keep_clear_of_insets() noexcept {
    const bridge::Insets insets{bridge::insets()};
    ImGuiViewportP* const viewport{static_cast<ImGuiViewportP*>(ImGui::GetMainViewport())};
    viewport->BuildWorkInsetMin.x += insets.left;
    viewport->BuildWorkInsetMin.y += insets.top;
    viewport->BuildWorkInsetMax.x += insets.right;
    viewport->BuildWorkInsetMax.y += insets.bottom;
}

void draw_frame(Session& session) noexcept {
    ImGuiIO& io{ImGui::GetIO()};
    // The on-screen keyboard comes up for a text field, and what is typed on it goes to ImGui.
    if (io.WantTextInput != session.keyboard) {
        session.keyboard = io.WantTextInput;
        bridge::show_keyboard(session.keyboard);
    }
    std::uint32_t character{bridge::next_character()};
    for (std::uint32_t count{0U}; (character != 0U) && (count < kMaxCharacters); ++count) {
        io.AddInputCharacter(character);
        character = bridge::next_character();
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    keep_clear_of_insets();
    session.app->draw();
    ImGui::Render();
    glViewport(0, 0, ANativeWindow_getWidth(session.glue->window), ANativeWindow_getHeight(session.glue->window));
    glClearColor(0.08F, 0.08F, 0.09F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    static_cast<void>(eglSwapBuffers(session.display.display, session.display.surface));

    // The screen stays on while connected, e.g. for the dashboard of widget mode.
    const bool connected{session.worker->state().connected};
    if (connected != session.screen_on) {
        session.screen_on = connected;
        ANativeActivity_setWindowFlags(session.glue->activity, connected ? kKeepScreenOn : 0U,
                                       connected ? 0U : kKeepScreenOn);
    }
}

/// Gives the Features tab its definitions: the simulator's, built in, then features.ini in the app's
/// directory on the shared storage.
void load_definitions(App& app) noexcept {
    // Static: the list of definitions is large.
    static elm327::gui::Definitions definitions{};
    elm327::gui::load_definitions(nullptr, definitions);
    app.set_features(definitions.features, definitions.source.view(), definitions.problem.view(),
                     definitions.directory.view());
}

}  // namespace

void android_main(android_app* const glue) {
    // The worker and the app embed large buffers: static storage, not the stack. They outlive one
    // activity: android_main() runs again when Android creates the next one in the same process.
    static Worker worker{};
    static App app{worker};
    Session session{};
    session.glue = glue;
    session.app = &app;
    session.worker = &worker;
    glue->userData = &session;
    glue->onAppCmd = &handle_command;
    glue->onInputEvent = &handle_input;

    if (!is_ok(bridge::begin(glue->activity))) {
        log_error("MainActivity lacks a method of the bridge: no Bluetooth");
    }
    const char* const files{(glue->activity->externalDataPath != nullptr) ? glue->activity->externalDataPath
                                                                           : glue->activity->internalDataPath};
    if (files != nullptr) {
        static_cast<void>(elm327::gui::set_config_directory(std::string_view{files}));
    }
    load_definitions(app);
    worker.set_link_opener(&bridge::open_link, nullptr);
    const bool started{is_ok(worker.start(&wake_up, glue->looper))};
    if (started) {
        elm327::gui::StartOptions options{};
        choose_device(options.settings);
        app.start(options);
    } else {
        log_error("cannot start the worker thread");
    }

    for (std::uint64_t frame{0U}; (glue->destroyRequested == 0) && (frame < kMaxFrames); ++frame) {
        int timeout{-1};  // No window: wait for one.
        if (session.drawing) {
            timeout = (session.frames_left > 0U) ? 0 : (app.active() ? kActiveWait : kIdleWait);
        }
        handle_events(glue, timeout);
        if (session.drawing && (glue->destroyRequested == 0)) {
            draw_frame(session);
            session.frames_left = (session.frames_left > 0U) ? (session.frames_left - 1U) : 0U;
        }
    }

    worker.stop();  // Closes the connection: MainActivity ends a Bluetooth link with it.
    stop_drawing(session);
    if (session.imgui) {
        ImGui::DestroyContext();
    }
    bridge::end();
}
