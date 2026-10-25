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
#include "elm327/version.hpp"
#include "sim/fake_elm327.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;

namespace {

/// What a run of elm327ctl printed, and its exit code.
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

/// Runs elm327ctl with @p arguments in-process, capturing its output in memory files.
Outcome run_cli(std::vector<std::string> arguments, const int cancel_descriptor = -1) {
    arguments.insert(arguments.begin(), "elm327ctl");
    std::vector<const char*> pointers{};
    for (const std::string& argument : arguments) {
        pointers.push_back(argument.c_str());
    }
    const int out_descriptor{::memfd_create("out", MFD_CLOEXEC)};
    const int err_descriptor{::memfd_create("err", MFD_CLOEXEC)};
    Outcome outcome{};
    if ((out_descriptor >= 0) && (err_descriptor >= 0)) {
        elm327::cli::Console out{out_descriptor};
        elm327::cli::Console err{err_descriptor};
        outcome.exit_code = elm327::cli::run(common::Span<const char* const>{pointers.data(), pointers.size()}, -1,
                                             out, err, cancel_descriptor);
        outcome.out = read_back(out_descriptor);
        outcome.err = read_back(err_descriptor);
    }
    static_cast<void>(::close(out_descriptor));
    static_cast<void>(::close(err_descriptor));
    return outcome;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE(cli_help_version_and_usage_errors) {
    const Outcome help{run_cli({"--help"})};
    CHECK(help.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(help.out, "Usage: elm327ctl [options] [command [arguments]]\n"));
    CHECK(contains(help.out, "  monitor PID...    read PIDs repeatedly until Ctrl+C (see -i, -n)\n"));
    CHECK(help.err.empty());

    const Outcome version{run_cli({"--version"})};
    CHECK(version.exit_code == elm327::cli::kExitSuccess);
    CHECK(version.out == "elm327ctl " + std::string{elm327::kVersion} + "\n");

    const Outcome usage{run_cli({"read"})};
    CHECK(usage.exit_code == elm327::cli::kExitUsage);
    CHECK(usage.out.empty());
    CHECK(usage.err ==
          "elm327ctl: missing PID, e.g. 0C\n"
          "Try 'elm327ctl --help' for more information.\n");
}

TEST_CASE(cli_device_errors) {
    const Outcome missing{run_cli({"-d", "/nonexistent/ttyUSB9", "info"})};
    CHECK(missing.exit_code == elm327::cli::kExitFailure);
    CHECK(missing.err ==
          "elm327ctl: /nonexistent/ttyUSB9: no such device (choose the adapter's serial device with -d)\n");

    const Outcome not_serial{run_cli({"-d", "/dev/null", "info"})};
    CHECK(not_serial.exit_code == elm327::cli::kExitFailure);
    CHECK(not_serial.err == "elm327ctl: /dev/null: not a serial device\n");
}

TEST_CASE(cli_info) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "info"})};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    CHECK(outcome.out ==
          "Adapter      ELM327 v1.5\n"
          "Description  OBDII to RS232 Interpreter\n"
          "Voltage      12.6V\n"
          "Protocol     AUTO, ISO 15765-4 (CAN 11/500)\n");
    CHECK(outcome.err.empty());
}

TEST_CASE(cli_info_ignition_off) {
    elm327::sim::Vehicle vehicle{elm327::sim::example_vehicle()};
    vehicle.ignition_on = false;
    elm327::sim::FakeElm327 fake{vehicle};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "info"})};
    CHECK(outcome.exit_code == elm327::cli::kExitFailure);
    CHECK(contains(outcome.out, "Voltage      12.6V\n"));
    CHECK(!contains(outcome.out, "Protocol"));
    CHECK(outcome.err == "elm327ctl: connecting to the vehicle: UNABLE TO CONNECT (is the ignition on?)\n");
}

TEST_CASE(cli_read) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "read", "0C", "d", "05", "01", "A6"})};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    CHECK(outcome.out ==
          "0C  Engine speed                       1726.00 rpm\n"
          "0D  Vehicle speed                      42 km/h\n"
          "05  Engine coolant temperature         83 degC\n"
          "01  Monitor status since DTCs cleared  82 07 E5 00\n"
          "A6  Odometer                           12345.6 km\n");
    CHECK(outcome.err.empty());

    // A PID the vehicle does not support, and one this program does not know.
    const Outcome unsupported{run_cli({"-d", fake.device_path(), "read", "0A", "FE"})};
    CHECK(unsupported.exit_code == elm327::cli::kExitFailure);
    CHECK(unsupported.out ==
          "0A  Fuel pressure  not supported by the vehicle\n"
          "FE  (unknown PID)  not supported by the vehicle\n");
}

TEST_CASE(cli_pids) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "pids"})};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(outcome.out, "01  Monitor status since DTCs cleared\n"));
    CHECK(contains(outcome.out, "0C  Engine speed [rpm]\n"));
    CHECK(contains(outcome.out, "A6  Odometer [km]\n"));
    CHECK(!contains(outcome.out, "PIDs supported"));
    CHECK(!contains(outcome.out, "0A  "));
}

TEST_CASE(cli_dtc) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "dtc"})};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    CHECK(outcome.out ==
          "MIL          on, 2 confirmed DTCs\n"
          "Stored       P0133 P0420\n"
          "Pending      P0171\n"
          "Permanent    P0420\n");

    elm327::sim::Vehicle vehicle{elm327::sim::example_vehicle()};
    vehicle.can = false;
    vehicle.pids[0x01U] = {0x00U, 0x07U, 0xE5U, 0x00U};
    vehicle.stored_dtcs = {};
    vehicle.pending_dtcs = {};
    vehicle.permanent_dtcs = {};
    elm327::sim::FakeElm327 clean{vehicle};
    REQUIRE(clean.valid());
    const Outcome none{run_cli({"-d", clean.device_path(), "dtc"})};
    CHECK(none.exit_code == elm327::cli::kExitSuccess);
    CHECK(none.out ==
          "MIL          off, 0 confirmed DTCs\n"
          "Stored       none\n"
          "Pending      none\n"
          "Permanent    none\n");
}

TEST_CASE(cli_vin) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "vin"})};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    CHECK(outcome.out == "1D4GP00R55B123456\n");
}

TEST_CASE(cli_raw) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome single{run_cli({"-d", fake.device_path(), "raw", "ATI"})};
    CHECK(single.exit_code == elm327::cli::kExitSuccess);
    CHECK(single.out == "ELM327 v1.5\n");

    const Outcome several{run_cli({"-d", fake.device_path(), "raw", "ATRV", "010D", "ATXYZ"})};
    CHECK(several.exit_code == elm327::cli::kExitFailure);  // The adapter rejected ATXYZ.
    CHECK(several.out ==
          "> ATRV\n"
          "12.6V\n"
          "> 010D\n"
          "SEARCHING...\n"
          "41 0D 2A\n"
          "> ATXYZ\n"
          "?\n");
}

TEST_CASE(cli_verbose) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-v", "-d", fake.device_path(), "raw", "ATRV"})};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    CHECK(outcome.out == "12.6V\n");
    CHECK(contains(outcome.err, "-> ATZ\n<- ATZ\n<- ELM327 v1.5\n-> ATE0\n<- ATE0\n<- OK\n-> ATL0\n<- OK\n"));
    CHECK(contains(outcome.err, "-> ATRV\n<- 12.6V\n"));
}

TEST_CASE(cli_monitor) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "-i", "10", "-n", "3", "monitor", "0C", "0D", "0A"})};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);
    const std::string legend{
        "# 0C  Engine speed [rpm]\n"
        "# 0D  Vehicle speed [km/h]\n"
        "# 0A  Fuel pressure [kPa] (not supported by the vehicle)\n"
        "  time [s]        0C        0D        0A\n"};
    CHECK(outcome.out.compare(0U, legend.size(), legend) == 0);
    std::size_t rows{0U};
    for (std::size_t position{outcome.out.find("   1726.00        42        --\n")}; position != std::string::npos;
         position = outcome.out.find("   1726.00        42        --\n", position + 1U)) {
        ++rows;
    }
    CHECK(rows == 3U);
    CHECK(outcome.err == "elm327ctl: 3 samples, 0 failed reads\n");
}

TEST_CASE(cli_monitor_stops_when_cancelled) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    test::Event event{};
    REQUIRE(event.descriptor() >= 0);
    std::thread canceller{[&event]() {
        std::this_thread::sleep_for(300ms);
        event.signal();
    }};
    // Without -n, monitor runs until it is cancelled.
    const Outcome outcome{run_cli({"-d", fake.device_path(), "-i", "20", "monitor", "0C"}, event.descriptor())};
    canceller.join();
    CHECK(outcome.exit_code == elm327::cli::kExitInterrupted);
    CHECK(contains(outcome.out, "1726.00\n"));
    CHECK(contains(outcome.err, " samples, 0 failed reads\n"));

    // The port was closed: the device can be opened again at once.
    const Outcome again{run_cli({"-d", fake.device_path(), "vin"})};
    CHECK(again.exit_code == elm327::cli::kExitSuccess);
}

TEST_CASE(cli_interrupted_while_connecting) {
    // The protocol search takes a second; the user stops the program before it ends.
    elm327::sim::Behaviour behaviour{};
    behaviour.search_delay = 1s;
    elm327::sim::FakeElm327 fake{elm327::sim::example_vehicle(), behaviour};
    REQUIRE(fake.valid());
    test::Event event{};
    REQUIRE(event.descriptor() >= 0);
    std::thread canceller{[&event]() {
        std::this_thread::sleep_for(200ms);
        event.signal();
    }};
    const Outcome outcome{run_cli({"-d", fake.device_path(), "dtc"}, event.descriptor())};
    canceller.join();
    CHECK(outcome.exit_code == elm327::cli::kExitInterrupted);
    CHECK(outcome.out.empty());
    CHECK(outcome.err == "elm327ctl: connecting to the vehicle: interrupted\n");
}

TEST_CASE(cli_read_only_blocks_raw_write) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "-r", "raw", "ATRV", "04"})};
    CHECK(outcome.exit_code == elm327::cli::kExitFailure);
    CHECK(contains(outcome.out, "12.6V"));  // The read command still went through.
    CHECK(contains(outcome.err, "04: refused in read-only mode"));
}

TEST_CASE(cli_clear_needs_confirmation_and_clears) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome unconfirmed{run_cli({"-d", fake.device_path(), "clear"})};
    CHECK(unconfirmed.exit_code == elm327::cli::kExitFailure);
    CHECK(contains(unconfirmed.err, "Pass -y"));

    const Outcome cleared{run_cli({"-d", fake.device_path(), "-y", "clear"})};
    CHECK(cleared.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(cleared.out, "Cleared the DTCs"));
}

TEST_CASE(cli_clear_refused_in_read_only) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "-r", "-y", "clear"})};
    CHECK(outcome.exit_code == elm327::cli::kExitFailure);
    CHECK(contains(outcome.err, "refused in read-only mode"));
    for (const std::string& command : fake.commands()) {
        CHECK(command.substr(0U, 2U) != "04");  // The clear never reached the vehicle.
    }
}

TEST_CASE(cli_uds_read_and_write_with_backup) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome read{run_cli({"-d", fake.device_path(), "uds", "read", "0407"})};
    CHECK(read.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(read.out, "ReadDataByIdentifier ok"));

    const Outcome written{run_cli({"-d", fake.device_path(), "-y", "uds", "write", "0407", "01"})};
    CHECK(written.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(written.out, "current value"));  // Reads and shows the old value first.
    CHECK(contains(written.out, "WriteDataByIdentifier ok"));
    CHECK(contains(written.out, "new value"));
}

TEST_CASE(cli_uds_write_refused_in_read_only) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const Outcome outcome{run_cli({"-d", fake.device_path(), "-r", "uds", "write", "0407", "01"})};
    CHECK(outcome.exit_code == elm327::cli::kExitFailure);
    CHECK(contains(outcome.err, "refused in read-only mode"));
    for (const std::string& command : fake.commands()) {
        CHECK(command.substr(0U, 2U) != "2E");  // The write never reached the vehicle.
    }
}

TEST_CASE(cli_uds_help) {
    const Outcome outcome{run_cli({"-d", "/dev/null", "uds", "help"})};
    CHECK(outcome.exit_code == elm327::cli::kExitSuccess);  // 'help' needs no adapter.
    CHECK(contains(outcome.out, "uds read DID"));
    CHECK(contains(outcome.out, "refused there before anything is sent"));
}

TEST_CASE(cli_coding_catalog) {
    const Outcome all{run_cli({"coding"})};
    CHECK(all.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(all.out, "Reference only"));
    CHECK(contains(all.out, "VAG"));
    CHECK(contains(all.out, "Renault"));
    CHECK(contains(all.out, "BMW"));

    const Outcome bmw{run_cli({"coding", "bmw"})};
    CHECK(bmw.exit_code == elm327::cli::kExitSuccess);
    CHECK(contains(bmw.out, "BMW"));
    CHECK(!contains(bmw.out, "VAG"));

    const Outcome bad{run_cli({"coding", "ferrari"})};
    CHECK(bad.exit_code == elm327::cli::kExitUsage);
    CHECK(contains(bad.err, "unknown make"));
}
