#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "cli/commands.hpp"
#include "cli/console.hpp"
#include "common/containers/span.hpp"
#include "common/time/clock.hpp"
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

std::string read_back(const int descriptor) {
    std::string text{};
    static_cast<void>(::lseek(descriptor, 0, SEEK_SET));
    std::array<char, 4096U> buffer{};
    for (ssize_t count{::read(descriptor, buffer.data(), buffer.size())}; count > 0;
         count = ::read(descriptor, buffer.data(), buffer.size())) {
        text.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return text;
}

/// Runs elm327ctl without a command, with @p arguments (options), and @p script as its input
/// through a pipe, capturing its output in memory files.
Outcome run_script(std::vector<std::string> arguments, const std::string& script, const int cancel_descriptor = -1) {
    arguments.insert(arguments.begin(), "elm327ctl");
    std::vector<const char*> pointers{};
    for (const std::string& argument : arguments) {
        pointers.push_back(argument.c_str());
    }
    std::array<int, 2U> pipe{{-1, -1}};
    const int out_descriptor{::memfd_create("out", MFD_CLOEXEC)};
    const int err_descriptor{::memfd_create("err", MFD_CLOEXEC)};
    Outcome outcome{};
    if ((::pipe2(pipe.data(), O_CLOEXEC) == 0) && (out_descriptor >= 0) && (err_descriptor >= 0)) {
        // The scripts are far smaller than the buffer of a pipe.
        const ssize_t written{::write(pipe[1U], script.data(), script.size())};
        static_cast<void>(written);
        static_cast<void>(::close(pipe[1U]));
        elm327::cli::Console out{out_descriptor};
        elm327::cli::Console err{err_descriptor};
        outcome.exit_code = elm327::cli::run(common::Span<const char* const>{pointers.data(), pointers.size()},
                                             pipe[0U], out, err, cancel_descriptor);
        outcome.out = read_back(out_descriptor);
        outcome.err = read_back(err_descriptor);
        static_cast<void>(::close(pipe[0U]));
    }
    static_cast<void>(::close(out_descriptor));
    static_cast<void>(::close(err_descriptor));
    return outcome;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

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
                       "Connection   not connected\n"));
    // A new baud rate closed the connection; the monitor connected again, at the old one.
    CHECK(contains(outcome.err, "Disconnected: the next command connects with the new setting.\n"));
    CHECK(contains(outcome.err, "elm327ctl: 2 samples, 0 failed reads\n"));
    CHECK(contains(outcome.err, "-> 010C\n"));  // verbose on.
    CHECK(contains(outcome.out, "Protocol     6 (ISO 15765-4 CAN (11 bit ID, 500 kbaud))\n"));
    CHECK(contains(outcome.out, "Count        2\nVerbose      off\nConnection   not connected\n"));
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

    const Outcome missing{run_script({"-d", "/nonexistent/ttyUSB9"}, "vin\nconnect\n")};
    CHECK(missing.exit_code == elm327::cli::kExitFailure);
    CHECK(missing.err ==
          "elm327ctl: /nonexistent/ttyUSB9: no such device (choose the adapter's serial device with 'set device' or "
          "'connect')\n"
          "elm327ctl: /nonexistent/ttyUSB9: no such device (choose the adapter's serial device with 'set device' or "
          "'connect')\n");

    // connect DEVICE changes the device setting.
    const Outcome connect{run_script({"-d", "/nonexistent/ttyUSB9"}, "connect " + fake.device_path() + "\nshow\n")};
    CHECK(connect.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(connect.out, "Device       " + fake.device_path() + "\n"));
    CHECK(contains(connect.out, "Connection   ELM327 v1.5 on " + fake.device_path() + "\n"));
}

TEST_CASE(shell_ctrl_c_stops_only_the_command) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    test::Event event{};
    REQUIRE(event.descriptor() >= 0);
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
    const int err_descriptor{::memfd_create("err", MFD_CLOEXEC)};
    REQUIRE(err_descriptor >= 0);

    std::int32_t exit_code{-1};
    std::thread shell{[&]() {
        const std::string device{fake.device_path()};
        const std::array<const char*, 3U> args{{"elm327ctl", "-d", device.c_str()}};
        elm327::cli::Console out{terminal};
        elm327::cli::Console err{err_descriptor};
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
    CHECK(contains(read_back(err_descriptor), "Connected to ELM327 v1.5"));
    static_cast<void>(::close(err_descriptor));
    static_cast<void>(::close(terminal));
    static_cast<void>(::close(master));
}
