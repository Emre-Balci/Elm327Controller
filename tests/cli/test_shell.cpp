#if defined(_WIN32)
#include <windows.h>

#include <mutex>
#include <string_view>
#else
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "cli/commands.hpp"
#include "cli/console.hpp"
#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/time/clock.hpp"
#include "elm327/controller/signals.hpp"
#include "sim/fake_elm327.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;

namespace {

/// What a run of the shell printed, and its exit code.
struct Outcome final {
    std::int32_t exit_code{-1};
    std::string out{};
    std::string err{};
};

/// Runs elm327ctl without a command, with @p arguments (options), and @p script as its input
/// through a pipe, capturing its output.
Outcome run_script(std::vector<std::string> arguments, const std::string& script,
                   const common::NativeHandle cancel_descriptor = common::kInvalidNativeHandle) {
    arguments.insert(arguments.begin(), "elm327ctl");
    std::vector<const char*> pointers{};
    for (const std::string& argument : arguments) {
        pointers.push_back(argument.c_str());
    }
    test::Pipe input{};
    const test::Capture out_capture{};
    const test::Capture err_capture{};
    Outcome outcome{};
    if (input.valid() && out_capture.valid() && err_capture.valid()) {
        input.write_and_close(script);
        elm327::cli::Console out{out_capture.descriptor()};
        elm327::cli::Console err{err_capture.descriptor()};
        outcome.exit_code = elm327::cli::run(common::Span<const char* const>{pointers.data(), pointers.size()},
                                             input.read_end(), out, err, cancel_descriptor);
        outcome.out = out_capture.text();
        outcome.err = err_capture.text();
    }
    return outcome;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

#if defined(_WIN32)
// A pseudo console (ConPTY) stands in for the user's console. A process has one console, so
// elm327ctl runs in a child: this program again, with this option and then the arguments.
constexpr std::string_view kConsoleChildOption{"--elm327ctl-on-console"};

/// In the child: runs elm327ctl as its main() does, on the console, with the arguments after the
/// option, and ends the process with the exit code. @return false in the parent.
bool run_if_console_child() {
    const bool child{(__argc > 1) && (std::string_view{__argv[1]} == kConsoleChildOption)};
    if (child) {
        std::vector<const char*> args{"elm327ctl"};
        for (int index{2}; index < __argc; ++index) {
            args.push_back(__argv[index]);
        }
        // The console itself, whatever the standard handles are.
        const HANDLE input{::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, 0U, nullptr)};
        const HANDLE output{::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, 0U, nullptr)};
        // Ctrl+C as in a console the user opened: a test runner may have started this program in a
        // process group of its own, which turns it off.
        static_cast<void>(::SetConsoleCtrlHandler(nullptr, FALSE));
        elm327::StopSignals stop{};
        static_cast<void>(stop.open());
        elm327::cli::Console out{output};
        elm327::cli::Console err{output};
        std::int32_t exit_code{elm327::cli::run(common::Span<const char* const>{args.data(), args.size()}, input, out,
                                                err, stop.descriptor())};
        const std::int32_t signal{stop.received()};
        if (signal != 0) {
            exit_code = 128 + signal;
        }
        ::ExitProcess(static_cast<UINT>(exit_code));
    }
    return child;
}

[[maybe_unused]] const bool g_console_child{run_if_console_child()};

/// elm327ctl with some arguments, in a child process on a pseudo console: the test types keys into
/// the console and reads what it shows. The console shows the screen, not the bytes written, so
/// look for words, not whole lines.
class ConsoleChild final {
public:
    explicit ConsoleChild(const std::vector<std::string>& arguments) {
        HANDLE input_read{nullptr};
        HANDLE output_write{nullptr};
        bool ok{(::CreatePipe(&input_read, &input_, nullptr, 0U) != FALSE) &&
                (::CreatePipe(&output_, &output_write, nullptr, 0U) != FALSE)};
        ok = ok && SUCCEEDED(::CreatePseudoConsole(COORD{100, 30}, input_read, output_write, 0U, &console_));
        // The pseudo console has the ends it needs now.
        close(input_read);
        close(output_write);
        SIZE_T size{0U};
        static_cast<void>(::InitializeProcThreadAttributeList(nullptr, 1U, 0U, &size));
        storage_.resize(size);
        attributes_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        ok = ok && (::InitializeProcThreadAttributeList(attributes_, 1U, 0U, &size) != FALSE);
        initialized_ = ok;
        ok = ok && (::UpdateProcThreadAttribute(attributes_, 0U, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console_,
                                                sizeof(console_), nullptr, nullptr) != FALSE);
        std::array<wchar_t, 32768U> program{};
        ok = ok && (::GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size())) != 0U);
        // None of the arguments has a space or a quote.
        std::wstring command{L"\"" + std::wstring{program.data()} + L"\" " + widen(kConsoleChildOption)};
        for (const std::string& argument : arguments) {
            command += L" " + widen(argument);
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes_;
        ok = ok && (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                                     nullptr, nullptr, &startup.StartupInfo, &child_) != FALSE);
        if (ok) {
            // The pseudo console stops while nobody reads what it shows: a thread reads all the time.
            reader_ = std::thread{[this]() {
                std::array<char, 1024U> buffer{};
                DWORD count{0U};
                while ((::ReadFile(output_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) !=
                        FALSE) &&
                       (count > 0U)) {
                    const std::lock_guard<std::mutex> lock{mutex_};
                    screen_.append(buffer.data(), count);
                }
            }};
        }
        started_ = ok;
    }

    ~ConsoleChild() {
        static_cast<void>(finish());
        if (initialized_) {
            ::DeleteProcThreadAttributeList(attributes_);
        }
        close(input_);
        close(output_);
    }

    ConsoleChild(const ConsoleChild&) = delete;
    ConsoleChild& operator=(const ConsoleChild&) = delete;
    ConsoleChild(ConsoleChild&&) = delete;
    ConsoleChild& operator=(ConsoleChild&&) = delete;

    [[nodiscard]] bool started() const {
        return started_;
    }

    /// Waits until @p text appears on the console, for at most a few seconds.
    [[nodiscard]] bool wait_for(const std::string& text) {
        const common::TimePoint deadline{common::MonotonicClock::now() + 10s};
        bool found{false};
        while ((!found) && (common::MonotonicClock::now() < deadline)) {
            {
                const std::lock_guard<std::mutex> lock{mutex_};
                found = screen_.find(text) != std::string::npos;
            }
            if (!found) {
                std::this_thread::sleep_for(20ms);
            }
        }
        return found;
    }

    /// Forgets what the console showed so far.
    void clear() {
        const std::lock_guard<std::mutex> lock{mutex_};
        screen_.clear();
    }

    void type(const std::string& keys) {
        DWORD written{0U};
        static_cast<void>(::WriteFile(input_, keys.data(), static_cast<DWORD>(keys.size()), &written, nullptr));
    }

    /// Waits for the child to end, at most a few seconds before it is killed, and closes the console.
    /// @return its exit code.
    std::uint32_t finish() {
        DWORD exit_code{0xFFFFFFFFU};
        if (child_.hProcess != nullptr) {
            if (::WaitForSingleObject(child_.hProcess, 10000U) != WAIT_OBJECT_0) {
                static_cast<void>(::TerminateProcess(child_.hProcess, 0xFFFFFFFFU));
                static_cast<void>(::WaitForSingleObject(child_.hProcess, 10000U));
            }
            static_cast<void>(::GetExitCodeProcess(child_.hProcess, &exit_code));
            close(child_.hProcess);
            close(child_.hThread);
            exit_code_ = exit_code;
        }
        if (console_ != nullptr) {
            ::ClosePseudoConsole(console_);  // Ends the output, and with it the reader.
            console_ = nullptr;
        }
        if (reader_.joinable()) {
            reader_.join();
        }
        return exit_code_;
    }

private:
    /// @p text, which is ASCII, in UTF-16.
    static std::wstring widen(const std::string_view text) {
        std::wstring wide{};
        for (const char character : text) {
            wide.push_back(static_cast<wchar_t>(character));
        }
        return wide;
    }

    static void close(HANDLE& handle) {
        if (handle != nullptr) {
            static_cast<void>(::CloseHandle(handle));
            handle = nullptr;
        }
    }

    HANDLE input_{nullptr};   ///< The test types into it.
    HANDLE output_{nullptr};  ///< What the console shows comes out of it.
    HPCON console_{nullptr};
    std::vector<std::uint8_t> storage_{};
    LPPROC_THREAD_ATTRIBUTE_LIST attributes_{nullptr};
    bool initialized_{false};
    PROCESS_INFORMATION child_{};
    std::uint32_t exit_code_{0xFFFFFFFFU};
    bool started_{false};
    std::thread reader_{};
    std::mutex mutex_{};
    std::string screen_{};
};
#endif

std::size_t count_of(const std::vector<std::string>& commands, const std::string& command) {
    std::size_t count{0U};
    for (const std::string& sent : commands) {
        count += (sent == command) ? 1U : 0U;
    }
    return count;
}

}  // namespace

TEST_CASE(shell_keeps_the_connection) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_script({"-d", fake.device_path()}, "info\nread 0C 0D\n\n# A comment.\nvin\ndtc\n")};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    CHECK(outcome.out ==
          "Adapter      ELM327 v1.5\n"
          "Description  OBDII to RS232 Interpreter\n"
          "Voltage      12.6V\n"
          "Protocol     AUTO, ISO 15765-4 (CAN 11/500)\n"
          "0C  Engine speed   1726.00 rpm\n"
          "0D  Vehicle speed  42 km/h\n"
          "1D4GP00R55B123456\n"
          "MIL          on, 2 confirmed DTCs\n"
          "Stored       P0133 P0420\n"
          "Pending      P0171\n"
          "Permanent    P0420\n");
    // Without a terminal there is no prompt, only the notice of the connection.
    CHECK(outcome.err == "Connected to ELM327 v1.5 on " + fake.device_path() + " at 38400 bit/s.\n");
    // One reset, and the supported PIDs once, for all the commands.
    CHECK(count_of(fake.commands(), "ATZ") == 1U);
    CHECK(count_of(fake.commands(), "0120") == 1U);
}

TEST_CASE(shell_settings) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_script({"-d", fake.device_path(), "-i", "50"},
                                     "connect\nset baud 115200\nset count 2\nset verbose on\nshow\nset baud 38400\n"
                                     "monitor 0C\nset verbose off\nset protocol 6\nset\ndisconnect\ndisconnect\n")};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    const std::string device{fake.device_path()};
    CHECK(contains(outcome.out, "Connected to ELM327 v1.5 on " + device + " at 38400 bit/s.\n"));
    CHECK(contains(outcome.out,
                   "Device       " + device +
                       "\n"
                       "Baud rate    115200 bit/s\n"
                       "Protocol     0 (automatic)\n"
                       "Timeout      5000 ms\n"
                       "Interval     50 ms\n"
                       "Count        2\n"
                       "Verbose      on\n"
                       "Read-only    off\n"
                       "Connection   not connected\n"));
    // A new baud rate closed the connection; the monitor connected again, at the old one.
    CHECK(contains(outcome.err, "Disconnected: the next command connects with the new setting.\n"));
    CHECK(contains(outcome.err, "elm327ctl: 2 samples, 0 failed reads\n"));
    CHECK(contains(outcome.err, "-> 010C\n"));  // verbose on.
    CHECK(contains(outcome.out, "Protocol     6 (ISO 15765-4 CAN (11 bit ID, 500 kbaud))\n"));
    CHECK(contains(outcome.out, "Count        2\nVerbose      off\nRead-only    off\nConnection   not connected\n"));
    CHECK(contains(outcome.out, "Not connected.\n"));
    CHECK(count_of(fake.commands(), "ATZ") == 2U);
}

TEST_CASE(shell_errors_and_exit_codes) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome errors{run_script({"-d", fake.device_path()},
                                    "fly\nread 0C XYZ\ninfo -b 9600\nset baud 12345\nhelp\nread 0A\n")};
    // The exit code is that of the last command: the PID is not supported.
    CHECK(errors.exit_code == elm327::cli::kExitFailure);
    CHECK(contains(errors.err, "elm327ctl: unknown command: 'fly'\n"));
    CHECK(contains(errors.err, "elm327ctl: invalid PID (1 or 2 hexadecimal digits): 'XYZ'\n"));
    CHECK(contains(errors.err, "elm327ctl: '-b' is a setting in the shell: use 'set baud'\n"));
    CHECK(contains(errors.err, "elm327ctl: unsupported baud rate: '12345'\n"));
    CHECK(contains(errors.out, "  set NAME VALUE    change a setting\n"));
    CHECK(contains(errors.out, "0A  Fuel pressure  not supported by the vehicle\n"));

    const Outcome quit{run_script({"-d", fake.device_path()}, "fly\nquit\nvin\n")};
    CHECK(quit.exit_code == elm327::cli::kExitUsage);
    CHECK(quit.out.empty());  // Nothing after quit runs.

    const Outcome nothing{run_script({}, "")};
    CHECK(nothing.exit_code == elm327::cli::kExitSuccess);
    CHECK(nothing.out.empty());
    CHECK(nothing.err.empty());

    const std::string missing_device{test::kMissingDevice};
    const Outcome missing{run_script({"-d", missing_device}, "vin\nconnect\n")};
    CHECK(missing.exit_code == elm327::cli::kExitFailure);
    const std::string no_such_device{"elm327ctl: " + missing_device +
                                     ": no such device (choose the adapter's serial device with 'set device' or "
                                     "'connect')\n"};
    CHECK(missing.err == no_such_device + no_such_device);

    // connect DEVICE changes the device setting.
    const Outcome connect{run_script({"-d", missing_device}, "connect " + fake.device_path() + "\nshow\n")};
    CHECK(connect.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(connect.out, "Device       " + fake.device_path() + "\n"));
    CHECK(contains(connect.out, "Connection   ELM327 v1.5 on " + fake.device_path() + "\n"));
}

TEST_CASE(shell_ctrl_c_stops_only_the_command) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    test::Event event{};
    REQUIRE(event.valid());
    std::thread canceller{[&event]() {
        std::this_thread::sleep_for(300ms);
        event.signal();
    }};
    // The monitor runs until Ctrl+C; the shell goes on with the next line.
    const Outcome outcome{
        run_script({"-d", fake.device_path(), "-i", "20"}, "monitor 0C\nvin\n", event.descriptor())};
    canceller.join();
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(outcome.out, "1726.00\n"));
    CHECK(contains(outcome.out, "1D4GP00R55B123456\n"));
    CHECK(contains(outcome.err, " samples, 0 failed reads\n"));
    CHECK(count_of(fake.commands(), "ATZ") == 1U);
}

TEST_CASE(shell_goes_on_after_losing_the_device) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const std::string device{fake.device_path()};
    std::thread unplug{[&fake]() {
        std::this_thread::sleep_for(300ms);
        fake.hang_up();
    }};
    // No command after the loss may connect again: the number of the fake's pseudo-terminal is
    // free, and a terminal opened meanwhile, maybe by another program, may have it.
    const Outcome outcome{run_script({"-d", device, "-i", "20"}, "monitor 0C\nshow\n")};
    unplug.join();
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);  // Of show.
    CHECK(contains(outcome.err, "elm327ctl: reading the PIDs: disconnected\n"));
    CHECK(contains(outcome.out, "Connection   not connected\n"));
}

#if defined(_WIN32)
TEST_CASE(shell_on_a_console) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    ConsoleChild shell{{"-d", fake.device_path()}};
    REQUIRE(shell.started());
    CHECK(shell.wait_for("elm327>"));
    shell.type("vi\t\r");  // Tab completes "vin".
    CHECK(shell.wait_for("1D4GP00R55B123456"));
    shell.clear();
    shell.type("bogus\x03");  // At the prompt Ctrl+C is a key: it discards the line.
    CHECK(shell.wait_for("^C"));
    shell.type("\x1b[A\r");  // Up brings back "vin".
    CHECK(shell.wait_for("1D4GP00R55B123456"));
    // While a command runs, Ctrl+C is a stop request (CTRL_C_EVENT): it stops the command only.
    shell.type("monitor -i 50 0C\r");
    CHECK(shell.wait_for("1726.00"));
    shell.type("\x03");
    CHECK(shell.wait_for("samples, 0 failed reads"));
    shell.clear();
    shell.type("vin\r");
    CHECK(shell.wait_for("1D4GP00R55B123456"));
    shell.type("\x04");  // Ctrl+D on an empty line ends the shell.
    CHECK(shell.finish() == 0U);
}

TEST_CASE(cli_ctrl_c_on_a_console) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    // Without -n, monitor runs until Ctrl+C, which the console sends as CTRL_C_EVENT.
    ConsoleChild cli{{"-d", fake.device_path(), "-i", "50", "monitor", "0C"}};
    REQUIRE(cli.started());
    CHECK(cli.wait_for("1726.00"));
    cli.type("\x03");
    CHECK(cli.wait_for("samples, 0 failed reads"));
    CHECK(cli.finish() == 130U);  // 128 + SIGINT, as main() reports it.
}
#else
TEST_CASE(shell_on_a_terminal) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    // A pseudo-terminal stands in for the user's terminal: the test types on its master side.
    const int master{::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)};
    REQUIRE(master >= 0);
    REQUIRE((::grantpt(master) == 0) && (::unlockpt(master) == 0));
    std::array<char, 128U> name{};
    REQUIRE(::ptsname_r(master, name.data(), name.size()) == 0);
    const int terminal{::open(name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
    REQUIRE(terminal >= 0);
    const test::Capture err_capture{};
    REQUIRE(err_capture.valid());

    std::int32_t exit_code{-1};
    std::thread shell{[&]() {
        const std::string device{fake.device_path()};
        const std::array<const char*, 3U> args{{"elm327ctl", "-d", device.c_str()}};
        elm327::cli::Console out{terminal};
        elm327::cli::Console err{err_capture.descriptor()};
        exit_code = elm327::cli::run(common::Span<const char* const>{args.data(), args.size()}, terminal, out, err, -1);
    }};

    // Reads what the shell wrote until @p text appears, for at most a few seconds.
    std::string screen{};
    const auto wait_for = [&](const std::string& text) {
        const common::TimePoint deadline{common::MonotonicClock::now() + 5s};
        while ((screen.find(text) == std::string::npos) && (common::MonotonicClock::now() < deadline)) {
            pollfd entry{master, POLLIN, 0};
            if (::poll(&entry, 1U, 50) > 0) {
                std::array<char, 1024U> buffer{};
                const ssize_t count{::read(master, buffer.data(), buffer.size())};
                if (count > 0) {
                    screen.append(buffer.data(), static_cast<std::size_t>(count));
                }
            }
        }
        return screen.find(text) != std::string::npos;
    };
    const auto type = [&](const std::string& keys) {
        const ssize_t written{::write(master, keys.data(), keys.size())};
        static_cast<void>(written);
    };

    // The terminal ends lines with "\r\n".
    CHECK(wait_for("Type 'help' for the commands, 'quit' to leave.\r\n"));
    CHECK(wait_for("elm327> "));
    type("vi\t\r");  // Tab completes "vin".
    CHECK(wait_for("1D4GP00R55B123456"));
    screen.clear();
    type("bogus\x03");  // Ctrl+C discards the line.
    CHECK(wait_for("^C\r\n"));
    type("\x1b[A\r");  // Up brings back "vin".
    CHECK(wait_for("1D4GP00R55B123456"));
    type("\x04");  // Ctrl+D on an empty line ends the shell.
    shell.join();
    CHECK(exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(err_capture.text(), "Connected to ELM327 v1.5"));
    static_cast<void>(::close(terminal));
    static_cast<void>(::close(master));
}
#endif

TEST_CASE(shell_read_only_refuses_writes) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_script({"-d", fake.device_path()},
                                     "set readonly on\nshow\nraw 04\nuds write 0407 01\nset readonly off\nraw ATRV\n")};
    CHECK(contains(outcome.out, "Read-only    on\n"));
    CHECK(contains(outcome.err, "refused in read-only mode"));
    CHECK(contains(outcome.out, "12.6V"));  // The read after turning read-only off still works.
    // While read-only was on, no write service ever reached the vehicle.
    for (const std::string& command : fake.commands()) {
        CHECK(command.substr(0U, 2U) != "04");
        CHECK(command.substr(0U, 2U) != "2E");
    }
}
