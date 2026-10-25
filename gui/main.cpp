/// @file main.cpp
/// @brief elm327gui: graphical front-end for ELM327 OBD-II adapters, drawn with Dear ImGui on GLFW
///        and OpenGL 3. See kUsage.
///
/// The main thread draws the window; the Worker talks to the adapter on a thread of its own, and
/// wakes the main thread (glfwPostEmptyEvent()) when it has something new. Ctrl+C in the terminal
/// that started the program closes the window like its close button, so the serial port is closed
/// cleanly (see StopSignals).

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"
#include "common/file/serial_port.hpp"
#include "common/protocol/text.hpp"
#include "common/time/clock.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/controller/signals.hpp"
#include "elm327/obd/pid.hpp"
#include "elm327/version.hpp"
#include "gui/app.hpp"
#include "gui/worker.hpp"

// The GLFW header comes without the system OpenGL headers (GLFW_INCLUDE_NONE): the OpenGL functions
// come from the loader of the ImGui backend, which finds them at run time.
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h"

namespace {

using elm327::gui::StartOptions;
using elm327::gui::Tab;

constexpr std::int32_t kExitSuccess{0};
constexpr std::int32_t kExitFailure{1};
constexpr std::int32_t kExitUsage{2};

constexpr std::array<std::string_view, 23U> kUsage{{
    "Usage: elm327gui [options]",
    "",
    "Graphical front-end for ELM327 OBD-II adapters: live data with plots, trouble codes, the VIN,",
    "and a terminal for AT commands. The options fill in the connection bar.",
    "",
    "Options:",
    "  -d, --device PATH     serial device (default /dev/ttyUSB0)",
    "  -b, --baud RATE       bit rate (default 38400)",
    "  -p, --protocol N      OBD protocol 0-9 or A-C (default 0: automatic)",
    "  -t, --timeout MS      reply timeout of a command in ms (default 5000)",
    "  -c, --connect         connect at the start",
    "  -m, --monitor PIDS    connect and monitor these PIDs, e.g. 0C,0D,05",
    "  -i, --interval MS     time between the samples of the monitor (default 1000)",
    "  -w, --widgets         widget mode: the live data as the gauges of a dashboard",
    "  -r, --read-only       safe mode: refuse anything that could change the vehicle",
    "      --tab NAME        open the tab live, codes, vehicle or terminal",
    "      --screenshot FILE save the window as a PPM image once it is idle, then exit",
    "  -h, --help            show this help",
    "      --version         show the version",
    "",
    "Without an adapter, run elm327sim and use the device it prints, e.g.",
    "  elm327gui -d /dev/pts/5 --monitor 0C,0D       (or --connect --widgets)",
    "Ctrl+C in this terminal closes the window.",
}};

/// Startup size of the window, before the scaling for the screen.
constexpr float kWindowWidth{1280.0F};
constexpr float kWindowHeight{800.0F};

/// A font that looks better than ImGui's built-in one, when it is installed (fonts-dejavu-core).
constexpr const char* kFontPath{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
constexpr float kFontSize{15.0F};

/// Frames per second while the worker is busy or monitoring; otherwise the window only redraws on
/// input and results, and every kIdleWait to check for Ctrl+C.
constexpr double kActiveWait{1.0 / 30.0};
constexpr double kIdleWait{0.25};

/// The screenshot: once the worker has been idle that long, or at the latest after kShotLatest.
constexpr common::Duration kShotIdle{std::chrono::milliseconds{1500}};
constexpr common::Duration kShotEarliest{std::chrono::seconds{3}};
constexpr common::Duration kShotLatest{std::chrono::seconds{20}};
constexpr std::size_t kMaxShotWidth{8192U};

/// Frames at most: a bound for the loop, years at any frame rate.
constexpr std::uint64_t kMaxFrames{std::numeric_limits<std::uint64_t>::max()};

/// What the command line asks for besides StartOptions.
struct Arguments final {
    StartOptions start{};
    const char* screenshot{nullptr};
    bool help{false};
    bool version{false};
};

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

/// Writes @p text to @p descriptor, e.g. stderr. Output is best effort.
void write_text(const std::int32_t descriptor, const std::string_view text) noexcept {
    const ssize_t written{::write(descriptor, text.data(), text.size())};
    static_cast<void>(written);
}

void print_error(const std::string_view reason, const std::string_view subject) noexcept {
    write_text(STDERR_FILENO, "elm327gui: ");
    write_text(STDERR_FILENO, reason);
    if (!subject.empty()) {
        write_text(STDERR_FILENO, ": '");
        write_text(STDERR_FILENO, subject);
        write_text(STDERR_FILENO, "'");
    }
    write_text(STDERR_FILENO, "\nTry 'elm327gui --help' for more information.\n");
}

bool parse_milliseconds(const std::string_view text, common::Duration& duration) noexcept {
    const common::Result<std::uint32_t> value{common::parse_unsigned<std::uint32_t>(span_of(text))};
    const bool valid{value.has_value() && (value.value() > 0U)};
    if (valid) {
        duration = std::chrono::milliseconds{value.value()};
    }
    return valid;
}

/// Parses "0C,0D,05" into @p pids.
bool parse_pid_list(const std::string_view text, common::ByteSet& pids) noexcept {
    const common::Span<const char> list{span_of(text)};
    bool valid{!list.empty()};
    std::size_t start{0U};
    for (std::size_t index{0U}; valid && (index <= list.size()); ++index) {
        if ((index == list.size()) || (list[index] == ',')) {
            const common::Span<const char> item{list.subspan(start, index - start)};
            const common::Result<std::uint8_t> pid{(item.size() <= 2U) ? common::parse_hex<std::uint8_t>(item)
                                                                       : common::Result<std::uint8_t>::failure(
                                                                             common::Status::kInvalidArgument)};
            valid = pid.has_value() && (pid.value() != 0U);
            pids = pids.with(pid.value_or(0U));
            start = index + 1U;
        }
    }
    return valid;
}

bool parse_tab(const std::string_view text, Tab& tab) noexcept {
    bool valid{true};
    if (text == "live") {
        tab = Tab::kLive;
    } else if (text == "codes") {
        tab = Tab::kTroubleCodes;
    } else if (text == "vehicle") {
        tab = Tab::kVehicle;
    } else if (text == "terminal") {
        tab = Tab::kTerminal;
    } else {
        valid = false;
    }
    return valid;
}

/// Applies the option @p name with @p value (nullptr when the option has none or it is missing).
/// @return false after printing a usage error. @p used tells whether the value was taken.
bool apply_option(const std::string_view name, const char* const value, Arguments& arguments, bool& used) noexcept {
    StartOptions& start{arguments.start};
    const std::string_view text{(value != nullptr) ? std::string_view{value} : std::string_view{}};
    bool valid{true};
    used = false;
    const bool takes_value{(name == "-d") || (name == "--device") || (name == "-b") || (name == "--baud") ||
                           (name == "-p") || (name == "--protocol") || (name == "-t") || (name == "--timeout") ||
                           (name == "-m") || (name == "--monitor") || (name == "-i") || (name == "--interval") ||
                           (name == "--tab") || (name == "--screenshot")};
    if (takes_value && (value == nullptr)) {
        print_error("option needs a value", name);
        valid = false;
    } else if ((name == "-d") || (name == "--device")) {
        valid = (!text.empty()) && start.settings.device.assign(text);
        if (!valid) {
            print_error("invalid device path", text);
        }
    } else if ((name == "-b") || (name == "--baud")) {
        const common::Result<std::uint32_t> rate{common::parse_unsigned<std::uint32_t>(span_of(text))};
        const common::Result<common::BaudRate> baud_rate{
            rate.has_value() ? common::baud_rate_from_bits_per_second(rate.value())
                             : common::Result<common::BaudRate>::failure(common::Status::kInvalidArgument)};
        valid = baud_rate.has_value() && elm327::gui::find_baud_index(baud_rate.value()).has_value();
        start.settings.baud_rate = baud_rate.value_or(common::BaudRate::k38400);
        if (!valid) {
            print_error("unsupported baud rate", text);
        }
    } else if ((name == "-p") || (name == "--protocol")) {
        const common::Result<char> protocol{elm327::parse_protocol(span_of(text))};
        valid = protocol.has_value();
        start.settings.protocol = protocol.value_or('0');
        if (!valid) {
            print_error("invalid protocol (0-9 or A-C)", text);
        }
    } else if ((name == "-t") || (name == "--timeout")) {
        valid = parse_milliseconds(text, start.settings.timeouts.command);
        if (!valid) {
            print_error("invalid timeout in ms", text);
        }
    } else if ((name == "-m") || (name == "--monitor")) {
        valid = parse_pid_list(text, start.monitor);
        if (!valid) {
            print_error("invalid PID list, e.g. 0C,0D", text);
        }
    } else if ((name == "-i") || (name == "--interval")) {
        valid = parse_milliseconds(text, start.interval);
        if (!valid) {
            print_error("invalid interval in ms", text);
        }
    } else if (name == "--tab") {
        valid = parse_tab(text, start.tab);
        if (!valid) {
            print_error("unknown tab (live, codes, vehicle or terminal)", text);
        }
    } else if (name == "--screenshot") {
        arguments.screenshot = value;
    } else if ((name == "-c") || (name == "--connect")) {
        start.connect = true;
    } else if ((name == "-w") || (name == "--widgets")) {
        start.widgets = true;
    } else if ((name == "-r") || (name == "--read-only")) {
        start.read_only = true;
    } else if ((name == "-h") || (name == "--help")) {
        arguments.help = true;
    } else if (name == "--version") {
        arguments.version = true;
    } else {
        print_error("unknown option", name);
        valid = false;
    }
    used = takes_value;
    return valid;
}

bool parse_arguments(const common::Span<const char* const> args, Arguments& arguments) noexcept {
    bool valid{true};
    for (std::size_t index{1U}; valid && (index < args.size()); ++index) {
        const char* const value{((index + 1U) < args.size()) ? args[index + 1U] : nullptr};
        bool used{false};
        valid = (args[index] != nullptr) && apply_option(std::string_view{args[index]}, value, arguments, used);
        if (used) {
            ++index;
        }
    }
    return valid;
}

void report_glfw_error(const int code, const char* const description) {
    static_cast<void>(code);
    write_text(STDERR_FILENO, "elm327gui: GLFW: ");
    write_text(STDERR_FILENO, (description != nullptr) ? std::string_view{description} : std::string_view{});
    write_text(STDERR_FILENO, "\n");
}

/// Wakes up the main loop: called by the worker's thread when it has published something.
void wake_up(void* /*context*/) noexcept {
    glfwPostEmptyEvent();
}

/// Writes the framebuffer, @p width x @p height pixels, to @p path as a binary PPM image, one row
/// at a time so that no buffer of the whole image is needed.
common::Status save_screenshot(const char* const path, const int width, const int height) noexcept {
    common::File file{};
    common::FileOptions options{};
    options.access = common::FileAccess::kWriteOnly;
    options.creation = common::FileCreation::kCreateOrTruncate;
    common::Status status{((width > 0) && (height > 0) && (static_cast<std::size_t>(width) <= kMaxShotWidth))
                              ? file.open(path, options)
                              : common::Status::kOutOfRange};
    if (is_ok(status)) {
        common::StaticString<32U> header{};
        static_cast<void>(header.append("P6\n") &&
                          common::append_unsigned(header, static_cast<std::uint32_t>(width)) && header.append(' ') &&
                          common::append_unsigned(header, static_cast<std::uint32_t>(height)) &&
                          header.append("\n255\n"));
        status = file.write_all(header.bytes());
    }
    // OpenGL counts the rows from the bottom, PPM from the top.
    static std::array<std::uint8_t, kMaxShotWidth * 4U> rgba{};
    static std::array<std::uint8_t, kMaxShotWidth * 3U> rgb{};
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const std::size_t pixels{static_cast<std::size_t>(width)};
    for (int row{height - 1}; is_ok(status) && (row >= 0); --row) {
        glReadPixels(0, row, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        for (std::size_t pixel{0U}; pixel < pixels; ++pixel) {
            rgb[(pixel * 3U)] = rgba[(pixel * 4U)];
            rgb[(pixel * 3U) + 1U] = rgba[(pixel * 4U) + 1U];
            rgb[(pixel * 3U) + 2U] = rgba[(pixel * 4U) + 2U];
        }
        status = file.write_all(common::Span<const std::uint8_t>{rgb.data(), pixels * 3U});
    }
    const common::Status closed{file.close()};
    return is_ok(status) ? closed : status;
}

/// Runs the window until it is closed. @return the exit code.
std::int32_t run_window(const Arguments& arguments, elm327::StopSignals& stop) noexcept {
    // The worker and the app embed large buffers: static storage, not the stack.
    static elm327::gui::Worker worker{};
    static elm327::gui::App app{worker};
    std::int32_t exit_code{kExitFailure};
    static_cast<void>(glfwSetErrorCallback(&report_glfw_error));
    if (glfwInit() == GLFW_TRUE) {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        const float scale{ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor())};
        common::StaticString<64U> title{};
        static_cast<void>(title.append("elm327gui ") && title.append(elm327::kVersion));
        GLFWwindow* const window{glfwCreateWindow(static_cast<int>(kWindowWidth * scale),
                                                  static_cast<int>(kWindowHeight * scale), title.c_str(), nullptr,
                                                  nullptr)};
        if (window != nullptr) {
            glfwMakeContextCurrent(window);
            glfwSwapInterval(1);  // Vsync.
            IMGUI_CHECKVERSION();
            static_cast<void>(ImGui::CreateContext());
            ImGuiIO& io{ImGui::GetIO()};
            io.IniFilename = nullptr;  // No imgui.ini in the working directory.
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::StyleColorsDark();
            ImGuiStyle& style{ImGui::GetStyle()};
            style.ScaleAllSizes(scale);
            style.FontScaleDpi = scale;
            style.FrameRounding = 3.0F;
            if (::access(kFontPath, R_OK) == 0) {
                static_cast<void>(io.Fonts->AddFontFromFileTTF(kFontPath, kFontSize));
            }
            static_cast<void>(ImGui_ImplGlfw_InitForOpenGL(window, true));
            static_cast<void>(ImGui_ImplOpenGL3_Init(nullptr));

            if (is_ok(worker.start(&wake_up, nullptr))) {
                app.start(arguments.start);
                exit_code = kExitSuccess;
                const common::TimePoint started{common::MonotonicClock::now()};
                common::TimePoint busy_until{started};
                bool done{false};
                for (std::uint64_t frame{0U}; (!done) && (frame < kMaxFrames); ++frame) {
                    glfwWaitEventsTimeout(app.active() ? kActiveWait : kIdleWait);
                    if (stop.received() != 0) {
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) == 0) {
                        ImGui_ImplOpenGL3_NewFrame();
                        ImGui_ImplGlfw_NewFrame();
                        ImGui::NewFrame();
                        app.draw();
                        ImGui::Render();
                        int width{0};
                        int height{0};
                        glfwGetFramebufferSize(window, &width, &height);
                        glViewport(0, 0, width, height);
                        glClearColor(0.08F, 0.08F, 0.09F, 1.0F);
                        glClear(GL_COLOR_BUFFER_BIT);
                        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                        const common::TimePoint now{common::MonotonicClock::now()};
                        const elm327::gui::State& state{worker.state()};
                        if ((state.activity != elm327::gui::Activity::kIdle) &&
                            (state.activity != elm327::gui::Activity::kMonitoring)) {
                            busy_until = now;
                        }
                        const bool settled{((now - started) >= kShotEarliest) && ((now - busy_until) >= kShotIdle)};
                        if ((arguments.screenshot != nullptr) && (settled || ((now - started) >= kShotLatest))) {
                            if (!is_ok(save_screenshot(arguments.screenshot, width, height))) {
                                print_error("cannot write the screenshot", std::string_view{arguments.screenshot});
                                exit_code = kExitFailure;
                            }
                            glfwSetWindowShouldClose(window, GLFW_TRUE);
                        }
                        glfwSwapBuffers(window);
                    }
                    done = glfwWindowShouldClose(window) != 0;
                }
                // Before GLFW goes away: the worker calls glfwPostEmptyEvent().
                worker.stop();
            } else {
                write_text(STDERR_FILENO, "elm327gui: cannot start the worker thread\n");
            }
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
        } else {
            write_text(STDERR_FILENO, "elm327gui: cannot open a window (is there a display? DISPLAY or WAYLAND_DISPLAY)\n");
        }
        glfwTerminate();
    } else {
        write_text(STDERR_FILENO, "elm327gui: cannot initialize GLFW (is there a display?)\n");
    }
    return exit_code;
}

}  // namespace

int main(const int argc, const char* const argv[]) {
    // Before any other thread starts, so that all of them leave the stop signals to the descriptor.
    elm327::StopSignals stop{};
    static_cast<void>(stop.open());
    Arguments arguments{};
    std::int32_t exit_code{kExitSuccess};
    if (!parse_arguments(common::Span<const char* const>{argv, static_cast<std::size_t>(argc)}, arguments)) {
        exit_code = kExitUsage;
    } else if (arguments.help) {
        for (const std::string_view line : kUsage) {
            write_text(STDOUT_FILENO, line);
            write_text(STDOUT_FILENO, "\n");
        }
    } else if (arguments.version) {
        write_text(STDOUT_FILENO, "elm327gui ");
        write_text(STDOUT_FILENO, elm327::kVersion);
        write_text(STDOUT_FILENO, "\n");
    } else {
        exit_code = run_window(arguments, stop);
        const std::int32_t signal{stop.received()};
        if (signal != 0) {
            exit_code = 128 + signal;  // Closed by Ctrl+C or another stop signal, as a shell expects.
        }
    }
    return exit_code;
}
