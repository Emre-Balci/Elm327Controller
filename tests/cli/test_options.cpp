#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "cli/console.hpp"
#include "cli/options.hpp"
#include "common/core/status.hpp"
#include "common/file/serial_port.hpp"
#include "elm327/adapter/adapter.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;

using elm327::cli::Command;

namespace {

/// A command line and its parse. The options point into the arguments, so both live together.
class CommandLine final {
public:
    explicit CommandLine(std::vector<std::string> arguments) : arguments_{std::move(arguments)} {
        arguments_.insert(arguments_.begin(), "elm327ctl");
        for (const std::string& argument : arguments_) {
            pointers_.push_back(argument.c_str());
        }
        elm327::cli::Line message{};
        status = elm327::cli::parse_options(common::Span<const char* const>{pointers_.data(), pointers_.size()},
                                            options, message);
        error = test::string_of(message.view());
    }
    CommandLine(const CommandLine&) = delete;
    CommandLine& operator=(const CommandLine&) = delete;
    CommandLine(CommandLine&&) = delete;
    CommandLine& operator=(CommandLine&&) = delete;
    ~CommandLine() = default;

    [[nodiscard]] std::string argument(const std::size_t index) const {
        return test::string_of(options.arguments[index]);
    }

    common::Status status{common::Status::kOk};
    elm327::cli::Options options{};
    std::string error{};

private:
    std::vector<std::string> arguments_;
    std::vector<const char*> pointers_{};
};

/// @return the error message of a command line that must not parse.
std::string error_of(std::vector<std::string> arguments) {
    const CommandLine line{std::move(arguments)};
    return (line.status == common::Status::kInvalidArgument) ? line.error : std::string{"(parsed)"};
}

}  // namespace

TEST_CASE(options_defaults) {
    const CommandLine line{{"info"}};
    REQUIRE(common::is_ok(line.status));
    CHECK(line.options.command == Command::kInfo);
    CHECK(std::string{line.options.device} == "/dev/ttyUSB0");
    CHECK(line.options.baud_rate == common::BaudRate::k38400);
    CHECK(line.options.protocol == '0');
    CHECK(line.options.timeout == 5s);
    CHECK(line.options.interval == 1s);
    CHECK(line.options.count == 0U);
    CHECK(!line.options.verbose);
    CHECK(line.options.arguments.empty());
}

TEST_CASE(options_values) {
    const CommandLine line{{"-d", "/dev/pts/7", "--baud", "115200", "-p", "a", "--timeout=250", "-i", "100", "--count",
                            "3", "-v", "monitor", "0C", "0d"}};
    REQUIRE(common::is_ok(line.status));
    CHECK(line.options.command == Command::kMonitor);
    CHECK(std::string{line.options.device} == "/dev/pts/7");
    CHECK(line.options.baud_rate == common::BaudRate::k115200);
    CHECK(line.options.protocol == 'A');
    CHECK(line.options.timeout == 250ms);
    CHECK(line.options.interval == 100ms);
    CHECK(line.options.count == 3U);
    CHECK(line.options.verbose);
    REQUIRE(line.options.arguments.size() == 2U);
    CHECK(line.argument(0U) == "0C");
    CHECK(line.argument(1U) == "0d");

    // Options may follow the command, and "--device=" carries its value.
    const CommandLine after{{"read", "0C", "--device=/dev/rfcomm0"}};
    REQUIRE(common::is_ok(after.status));
    CHECK(std::string{after.options.device} == "/dev/rfcomm0");
    REQUIRE(after.options.arguments.size() == 1U);

    // After "--", everything is an argument.
    const CommandLine raw{{"raw", "--", "-v", "ATI"}};
    REQUIRE(common::is_ok(raw.status));
    CHECK(!raw.options.verbose);
    REQUIRE(raw.options.arguments.size() == 2U);
    CHECK(raw.argument(0U) == "-v");
}

TEST_CASE(options_help_and_version) {
    CHECK(CommandLine{{"--help"}}.options.command == Command::kHelp);
    CHECK(CommandLine{{"help"}}.options.command == Command::kHelp);
    CHECK((CommandLine{{"read", "-h"}}.options.command == Command::kHelp));  // No PID needed for help.
    CHECK(CommandLine{{"--version"}}.options.command == Command::kVersion);
    CHECK(common::is_ok(CommandLine{{"--version"}}.status));
}

TEST_CASE(options_errors) {
    CHECK(error_of({}) == "missing command");
    CHECK(error_of({"fly"}) == "unknown command: 'fly'");
    CHECK(error_of({"--bogus", "info"}) == "unknown option: '--bogus'");
    CHECK(error_of({"-x", "info"}) == "unknown option: '-x'");
    CHECK(error_of({"-b", "12345", "info"}) == "unsupported baud rate: '12345'");
    CHECK(error_of({"info", "-b"}) == "option needs a value: '--baud'");
    CHECK(error_of({"-p", "D", "info"}) == "invalid protocol (0-9 or A-C): 'D'");
    CHECK(error_of({"-p", "10", "info"}) == "invalid protocol (0-9 or A-C): '10'");
    CHECK(error_of({"-t", "0", "info"}) == "invalid timeout in ms: '0'");
    CHECK(error_of({"-i", "-5", "monitor", "0C"}) == "invalid interval in ms: '-5'");
    CHECK(error_of({"-n", "x", "monitor", "0C"}) == "invalid count: 'x'");
    CHECK(error_of({"--verbose=1", "info"}) == "option takes no value: '--verbose'");
    CHECK(error_of({"--device=", "info"}) == "empty device path: '--device'");
    CHECK(error_of({"read"}) == "missing PID, e.g. 0C");
    CHECK(error_of({"read", "0C", "XYZ"}) == "invalid PID (1 or 2 hexadecimal digits): 'XYZ'");
    CHECK(error_of({"monitor", "100"}) == "invalid PID (1 or 2 hexadecimal digits): '100'");
    CHECK(error_of({"info", "extra"}) == "unexpected argument: 'extra'");
    CHECK(error_of({"raw"}) == "missing command for the adapter, e.g. ATI");
    const std::string long_command(elm327::Adapter::kMaxCommandLength + 1U, 'A');
    CHECK(error_of({"raw", long_command}) == "command too long for the adapter: '" + long_command + "'");

    std::vector<std::string> many{"read"};
    for (std::size_t index{0U}; index <= elm327::cli::Options::kMaxArguments; ++index) {
        many.push_back("0C");
    }
    CHECK(error_of(many) == "too many arguments: '0C'");
}

TEST_CASE(options_parse_pid) {
    CHECK(elm327::cli::parse_pid(test::span_of("0C")).value() == 0x0CU);
    CHECK(elm327::cli::parse_pid(test::span_of("c")).value() == 0x0CU);
    CHECK(elm327::cli::parse_pid(test::span_of("FF")).value() == 0xFFU);
    CHECK(!elm327::cli::parse_pid(test::span_of("")).has_value());
    CHECK(!elm327::cli::parse_pid(test::span_of("100")).has_value());
    CHECK(!elm327::cli::parse_pid(test::span_of("0x0C")).has_value());
    CHECK(!elm327::cli::parse_pid(test::span_of("G1")).has_value());
}
