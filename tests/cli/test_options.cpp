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
    CHECK(line.options.connection.device.view() == "/dev/ttyUSB0");
    CHECK(line.options.connection.baud_rate == common::BaudRate::k38400);
    CHECK(line.options.connection.protocol == '0');
    CHECK(line.options.connection.timeouts.command == 5s);
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
    CHECK(line.options.connection.device.view() == "/dev/pts/7");
    CHECK(line.options.connection.baud_rate == common::BaudRate::k115200);
    CHECK(line.options.connection.protocol == 'A');
    CHECK(line.options.connection.timeouts.command == 250ms);
    CHECK(line.options.interval == 100ms);
    CHECK(line.options.count == 3U);
    CHECK(line.options.verbose);
    REQUIRE(line.options.arguments.size() == 2U);
    CHECK(line.argument(0U) == "0C");
    CHECK(line.argument(1U) == "0d");

    // Options may follow the command, and "--device=" carries its value.
    const CommandLine after{{"read", "0C", "--device=/dev/rfcomm0"}};
    REQUIRE(common::is_ok(after.status));
    CHECK(after.options.connection.device.view() == "/dev/rfcomm0");
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

TEST_CASE(options_shell_without_command) {
    // Without a command the options become the settings of the shell.
    const CommandLine line{{"-d", "/dev/pts/3", "-v"}};
    REQUIRE(common::is_ok(line.status));
    CHECK(line.options.command == Command::kShell);
    CHECK(line.options.connection.device.view() == "/dev/pts/3");
    CHECK(line.options.verbose);
    CHECK(CommandLine{{}}.options.command == Command::kShell);

    // The shell's own commands do not run from the command line.
    CHECK(error_of({"connect"}) == "'connect' works in the shell only (start elm327ctl without a command)");
    CHECK(error_of({"quit"}) == "'quit' works in the shell only (start elm327ctl without a command)");
    const std::string long_path(elm327::ConnectionSettings::kMaxDeviceLength + 1U, 'x');
    CHECK(error_of({"-d", long_path}) == "device path too long: '" + long_path + "'");
}

namespace {

/// A line of the shell and its parse, starting from @p settings.
struct ShellLine final {
    ShellLine(const std::string& line, const elm327::cli::Options& settings) : text{line} {
        elm327::cli::Line message{};
        status = elm327::cli::split_words(test::span_of(text), words, message);
        if (common::is_ok(status)) {
            status = elm327::cli::parse_line(words, settings, options, message);
        }
        error = test::string_of(message.view());
    }

    std::string text;
    elm327::cli::Words words{};
    common::Status status{common::Status::kOk};
    elm327::cli::Options options{};
    std::string error{};
};

}  // namespace

TEST_CASE(options_split_words) {
    elm327::cli::Words words{};
    elm327::cli::Line error{};
    REQUIRE(common::is_ok(elm327::cli::split_words(test::span_of("  read\t0C   0D  # engine"), words, error)));
    REQUIRE(words.size() == 3U);
    CHECK(test::string_of(words[0U]) == "read");
    CHECK(test::string_of(words[2U]) == "0D");
    REQUIRE(common::is_ok(elm327::cli::split_words(test::span_of("# only a comment"), words, error)));
    CHECK(words.empty());

    std::string many{};
    for (std::size_t index{0U}; index <= elm327::cli::Words::capacity(); ++index) {
        many += "0C ";
    }
    CHECK(elm327::cli::split_words(test::span_of(many), words, error) == common::Status::kInvalidArgument);
    CHECK(test::string_of(error.view()) == "too many words");
}

TEST_CASE(options_shell_lines) {
    elm327::cli::Options settings{};
    REQUIRE(settings.connection.device.assign("/dev/pts/9"));
    settings.interval = 2s;

    // Options on a line apply to its command, on top of the settings.
    const ShellLine monitor{"monitor -i 250 -n 10 0C 0D", settings};
    REQUIRE(common::is_ok(monitor.status));
    CHECK(monitor.options.command == Command::kMonitor);
    CHECK(monitor.options.interval == 250ms);
    CHECK(monitor.options.count == 10U);
    CHECK(monitor.options.connection.device.view() == "/dev/pts/9");
    CHECK(monitor.options.arguments.size() == 2U);

    const ShellLine read{"read 0C", settings};
    REQUIRE(common::is_ok(read.status));
    CHECK(read.options.interval == 2s);

    CHECK((ShellLine{"", settings}.options.command == Command::kNone));
    CHECK((ShellLine{"   # nothing", settings}.options.command == Command::kNone));
    CHECK((ShellLine{"exit", settings}.options.command == Command::kQuit));
    CHECK((ShellLine{"-h", settings}.options.command == Command::kHelp));
    CHECK((ShellLine{"connect /dev/rfcomm0", settings}.options.command == Command::kConnect));
    CHECK((ShellLine{"set baud 9600", settings}.options.command == Command::kSet));
    CHECK((ShellLine{"set", settings}.options.command == Command::kSet));

    // The connection settings are changed with 'set' in the shell.
    CHECK((ShellLine{"info -d /dev/pts/1", settings}.error == "'-d' is a setting in the shell: use 'set device'"));
    CHECK((ShellLine{"-b 9600 info", settings}.error == "'-b' is a setting in the shell: use 'set baud'"));
    CHECK((ShellLine{"-v", settings}.error == "missing command"));
    CHECK((ShellLine{"fly", settings}.error == "unknown command: 'fly'"));
    CHECK((ShellLine{"set baud", settings}.error == "missing value for the setting: 'baud'"));
    CHECK((ShellLine{"set baud 9600 now", settings}.error == "unexpected argument: 'now'"));
    CHECK((ShellLine{"connect a b", settings}.error == "unexpected argument: 'b'"));
    CHECK((ShellLine{"quit now", settings}.error == "unexpected argument: 'now'"));
    CHECK((ShellLine{"read", settings}.error == "missing PID, e.g. 0C"));
}

TEST_CASE(options_apply_setting) {
    elm327::cli::Options options{};
    elm327::cli::Line error{};
    using elm327::cli::apply_setting;
    REQUIRE(common::is_ok(apply_setting(test::span_of("device"), test::span_of("/dev/rfcomm0"), options, error)));
    CHECK(options.connection.device.view() == "/dev/rfcomm0");
    REQUIRE(common::is_ok(apply_setting(test::span_of("baud"), test::span_of("115200"), options, error)));
    CHECK(options.connection.baud_rate == common::BaudRate::k115200);
    REQUIRE(common::is_ok(apply_setting(test::span_of("protocol"), test::span_of("b"), options, error)));
    CHECK(options.connection.protocol == 'B');
    REQUIRE(common::is_ok(apply_setting(test::span_of("timeout"), test::span_of("750"), options, error)));
    CHECK(options.connection.timeouts.command == 750ms);
    REQUIRE(common::is_ok(apply_setting(test::span_of("interval"), test::span_of("40"), options, error)));
    CHECK(options.interval == 40ms);
    REQUIRE(common::is_ok(apply_setting(test::span_of("count"), test::span_of("0"), options, error)));
    CHECK(options.count == 0U);
    REQUIRE(common::is_ok(apply_setting(test::span_of("verbose"), test::span_of("on"), options, error)));
    CHECK(options.verbose);
    REQUIRE(common::is_ok(apply_setting(test::span_of("verbose"), test::span_of("off"), options, error)));
    CHECK(!options.verbose);

    const auto failure = [](const std::string& name, const std::string& value) {
        elm327::cli::Options scratch{};
        elm327::cli::Line message{};
        const common::Status status{apply_setting(test::span_of(name), test::span_of(value), scratch, message)};
        return (status == common::Status::kInvalidArgument) ? test::string_of(message.view()) : std::string{"(set)"};
    };
    CHECK(failure("baud", "12345") == "unsupported baud rate: '12345'");
    CHECK(failure("protocol", "D") == "invalid protocol (0-9 or A-C): 'D'");
    CHECK(failure("verbose", "maybe") == "invalid value for verbose (on or off): 'maybe'");
    CHECK(failure("speed", "1") ==
          "unknown setting (device, baud, protocol, timeout, interval, count, verbose): 'speed'");

    CHECK(elm327::cli::is_connection_setting(test::span_of("device")));
    CHECK(elm327::cli::is_connection_setting(test::span_of("timeout")));
    CHECK(!elm327::cli::is_connection_setting(test::span_of("interval")));
    CHECK(!elm327::cli::is_connection_setting(test::span_of("verbose")));
}
