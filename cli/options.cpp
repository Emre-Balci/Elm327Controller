#include "cli/options.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/protocol/text.hpp"
#include "elm327/adapter/adapter.hpp"

namespace elm327 {
namespace cli {
namespace {

enum class OptionId : std::uint8_t {
    kDevice,
    kBaud,
    kProtocol,
    kTimeout,
    kInterval,
    kCount,
    kVerbose,
    kReadOnly,
    kYes,
    kHelp,
    kVersion,
};

struct OptionSpec final {
    OptionId id;
    std::string_view short_name;  ///< Empty if the option has none.
    std::string_view long_name;
    bool takes_value;
    std::string_view setting;  ///< The name of the option as a setting of the shell; empty if it is none.
    bool connection;           ///< A setting of the connection: on a line of the shell, use 'set' instead.
};

constexpr std::array<OptionSpec, 11U> kOptionSpecs{{
    {OptionId::kDevice, "-d", "--device", true, "device", true},
    {OptionId::kBaud, "-b", "--baud", true, "baud", true},
    {OptionId::kProtocol, "-p", "--protocol", true, "protocol", true},
    {OptionId::kTimeout, "-t", "--timeout", true, "timeout", true},
    {OptionId::kInterval, "-i", "--interval", true, "interval", false},
    {OptionId::kCount, "-n", "--count", true, "count", false},
    {OptionId::kVerbose, "-v", "--verbose", false, "verbose", false},
    {OptionId::kReadOnly, "-r", "--read-only", false, "readonly", false},
    {OptionId::kYes, "-y", "--yes", false, "", false},
    {OptionId::kHelp, "-h", "--help", false, "", false},
    {OptionId::kVersion, "", "--version", false, "", false},
}};

struct CommandSpec final {
    Command command;
    std::string_view name;
    bool shell_only;
};

constexpr std::array<CommandSpec, 17U> kCommandSpecs{{
    {Command::kInfo, "info", false},
    {Command::kPids, "pids", false},
    {Command::kRead, "read", false},
    {Command::kMonitor, "monitor", false},
    {Command::kDtc, "dtc", false},
    {Command::kVin, "vin", false},
    {Command::kRaw, "raw", false},
    {Command::kClear, "clear", false},
    {Command::kUds, "uds", false},
    {Command::kCoding, "coding", false},
    {Command::kHelp, "help", false},
    {Command::kConnect, "connect", true},
    {Command::kDisconnect, "disconnect", true},
    {Command::kSet, "set", true},
    {Command::kShow, "show", true},
    {Command::kQuit, "quit", true},
    {Command::kQuit, "exit", true},
}};

/// The names of kCommandSpecs, for completion.
constexpr std::array<std::string_view, kCommandSpecs.size()> kCommandNames{{
    "info", "pids",       "read", "monitor", "dtc", "vin",  "raw",  "clear", "uds",
    "coding", "help",     "connect", "disconnect", "set", "show", "quit", "exit",
}};

/// The settings of kOptionSpecs, for completion.
constexpr std::array<std::string_view, 8U> kSettingNames{{
    "device", "baud", "protocol", "timeout", "interval", "count", "verbose", "readonly",
}};

/// The values of 'set verbose'.
constexpr std::array<std::string_view, 4U> kOnWords{{"on", "yes", "true", "1"}};
constexpr std::array<std::string_view, 4U> kOffWords{{"off", "no", "false", "0"}};

constexpr std::array<std::string_view, 32U> kUsage{{
    "Usage: elm327ctl [options] [command [arguments]]",
    "",
    "Talks to an ELM327 OBD-II adapter on a serial port. Without a command, elm327ctl starts an",
    "interactive shell, which keeps the connection open between commands (type 'help' there).",
    "",
    "Commands:",
    "  info              adapter identity, supply voltage and the vehicle's protocol",
    "  pids              list the service 01 PIDs the vehicle supports",
    "  read PID...       read service 01 PIDs once, e.g. 'read 0C 0D 05'",
    "  monitor PID...    read PIDs repeatedly until Ctrl+C (see -i, -n)",
    "  dtc               read the MIL status and the stored, pending and permanent DTCs",
    "  vin               read the vehicle identification number",
    "  clear             clear the DTCs and turn the MIL off (service 04; needs -y)",
    "  raw COMMAND...    send AT commands or OBD requests and print the replies",
    "  uds SUBCOMMAND    expert mode: UDS/KWP requests to an ECU (try 'uds help')",
    "  coding [MAKE]     reference list of common owner codings (vag, renault, bmw)",
    "  help              show this help",
    "",
    "Options:",
    "  -d, --device PATH   serial device (default /dev/ttyUSB0)",
    "  -b, --baud RATE     bit rate (default 38400)",
    "  -p, --protocol N    OBD protocol 0-9 or A-C (default 0: automatic)",
    "  -t, --timeout MS    reply timeout of a command in ms (default 5000)",
    "  -i, --interval MS   monitor: time between samples in ms (default 1000)",
    "  -n, --count N       monitor: stop after N samples (default 0: until Ctrl+C)",
    "  -r, --read-only     safe mode: refuse any command that could change the vehicle",
    "  -y, --yes           confirm a write (clear, uds write) without prompting",
    "  -v, --verbose       print the traffic with the adapter on stderr",
    "  -h, --help          show this help",
    "      --version       show the version",
    "",
    "In the shell, the options are its settings at the start (change them with 'set').",
}};

constexpr std::array<std::string_view, 40U> kShellHelp{{
    "Commands:",
    "  connect [DEVICE]  connect to the adapter, at DEVICE if given (it becomes the device setting)",
    "  disconnect        close the connection",
    "  info              adapter identity, supply voltage and the vehicle's protocol",
    "  pids              list the service 01 PIDs the vehicle supports",
    "  read PID...       read service 01 PIDs once, e.g. 'read 0C 0D 05'",
    "  monitor PID...    read PIDs repeatedly until Ctrl+C (see the interval and count settings)",
    "  dtc               read the MIL status and the stored, pending and permanent DTCs",
    "  vin               read the vehicle identification number",
    "  clear             clear the DTCs and turn the MIL off (service 04; needs -y)",
    "  raw COMMAND...    send AT commands or OBD requests and print the replies",
    "  uds SUBCOMMAND    expert mode: UDS/KWP requests to an ECU (try 'uds help')",
    "  coding [MAKE]     reference list of common owner codings (vag, renault, bmw)",
    "  set NAME VALUE    change a setting",
    "  show              show the settings and the connection (also 'set' alone)",
    "  help              show this help",
    "  quit              leave the shell (also 'exit' or Ctrl+D)",
    "",
    "Settings:",
    "  device PATH       serial device, e.g. /dev/ttyUSB0 or /dev/rfcomm0",
    "  baud RATE         bit rate, e.g. 38400, 9600 or 115200",
    "  protocol N        OBD protocol 0-9 or A-C (0: automatic)",
    "  timeout MS        reply timeout of a command in ms",
    "  interval MS       monitor: time between samples in ms",
    "  count N           monitor: stop after N samples (0: until Ctrl+C)",
    "  verbose on|off    print the traffic with the adapter",
    "  readonly on|off   safe mode: refuse any command that could change the vehicle",
    "",
    "The commands that need the adapter connect to it first, and the connection stays open. A",
    "change of device, baud, protocol or timeout applies from the next connection.",
    "",
    "In read-only mode (readonly on, or -r) only reads reach the vehicle; writes such as clear,",
    "uds write and OBD service 04 are refused before they are sent.",
    "",
    "Options apply to one command, e.g. 'monitor -i 250 -n 10 0C 0D' or 'read -v 0C'.",
    "",
    "Keys: Ctrl+C stops a command or discards the line, Tab completes commands and settings,",
    "Up and Down browse the history, Ctrl+A and Ctrl+E go to the start and the end of the line,",
    "Ctrl+U, Ctrl+K and Ctrl+W delete before the cursor, after it and the word before it, and",
    "Ctrl+L clears the screen.",
}};

// Too few lines do not compile; too many would leave empty lines at the end.
static_assert(!kUsage.back().empty(), "kUsage has more entries than lines");
static_assert(!kShellHelp.back().empty(), "kShellHelp has more entries than lines");

/// Where the parsed words come from.
enum class Source : std::uint8_t {
    kCommandLine,
    kShell,
};

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

std::string_view view_of(const common::Span<const char> text) noexcept {
    return std::string_view{text.data(), text.size()};
}

/// Writes "<reason>: '<subject>'" to @p error, or the reason alone without a subject.
/// @return kInvalidArgument.
common::Status fail(Line& error, const std::string_view reason, const common::Span<const char> subject) noexcept {
    error.text(reason);
    if (!subject.empty()) {
        error.text(": '").text(subject).character('\'');
    }
    return common::Status::kInvalidArgument;
}

/// A number of milliseconds above 0.
common::Status parse_milliseconds(const common::Span<const char> text, common::Duration& duration) noexcept {
    const common::Result<std::uint32_t> value{common::parse_unsigned<std::uint32_t>(text)};
    const bool valid{value.has_value() && (value.value() > 0U)};
    if (valid) {
        duration = std::chrono::milliseconds{value.value()};
    }
    return valid ? common::Status::kOk : common::Status::kInvalidArgument;
}

/// @return true if @p word is one of @p words.
template <std::size_t Size>
bool is_one_of(const common::Span<const char> word, const std::array<std::string_view, Size>& words) noexcept {
    bool found{false};
    for (std::size_t index{0U}; (!found) && (index < words.size()); ++index) {
        found = common::equals(word, words[index]);
    }
    return found;
}

/// Sets the option @p spec, one that takes a value, to @p value.
common::Status apply_value(const OptionSpec& spec, const common::Span<const char> value, Options& options,
                           Line& error) noexcept {
    common::Status status{common::Status::kOk};
    switch (spec.id) {
        case OptionId::kDevice:
            if (value.empty()) {
                status = fail(error, "empty device path", span_of(spec.long_name));
            } else if (!options.connection.device.assign(view_of(value))) {
                status = fail(error, "device path too long", value);
            } else {
                // Set.
            }
            break;
        case OptionId::kBaud: {
            const common::Result<std::uint32_t> rate{common::parse_unsigned<std::uint32_t>(value)};
            const common::Result<common::BaudRate> baud_rate{
                rate.has_value() ? common::baud_rate_from_bits_per_second(rate.value())
                                 : common::Result<common::BaudRate>::failure(common::Status::kInvalidArgument)};
            if (baud_rate.has_value()) {
                options.connection.baud_rate = baud_rate.value();
            } else {
                status = fail(error, "unsupported baud rate", value);
            }
            break;
        }
        case OptionId::kProtocol: {
            const common::Result<char> protocol{parse_protocol(value)};
            if (protocol.has_value()) {
                options.connection.protocol = protocol.value();
            } else {
                status = fail(error, "invalid protocol (0-9 or A-C)", value);
            }
            break;
        }
        case OptionId::kTimeout:
            if (!is_ok(parse_milliseconds(value, options.connection.timeouts.command))) {
                status = fail(error, "invalid timeout in ms", value);
            }
            break;
        case OptionId::kInterval:
            if (!is_ok(parse_milliseconds(value, options.interval))) {
                status = fail(error, "invalid interval in ms", value);
            }
            break;
        case OptionId::kCount: {
            const common::Result<std::uint32_t> count{common::parse_unsigned<std::uint32_t>(value)};
            if (count.has_value()) {
                options.count = count.value();
            } else {
                status = fail(error, "invalid count", value);
            }
            break;
        }
        case OptionId::kVerbose:
        case OptionId::kReadOnly:
        case OptionId::kYes:
        case OptionId::kHelp:
        case OptionId::kVersion:
            status = fail(error, "option takes no value", span_of(spec.long_name));
            break;
    }
    return status;
}

/// Consumes the words of the command line or of a shell line, one at a time.
class Parser final {
public:
    Parser(Options& options, Line& error, const Source source) noexcept
        : options_{&options}, error_{&error}, source_{source} {}

    common::Status take(common::Span<const char> argument) noexcept;
    common::Status finish() noexcept;

private:
    common::Status take_option(common::Span<const char> argument) noexcept;
    common::Status take_word(common::Span<const char> word) noexcept;
    common::Status apply(const OptionSpec& spec, common::Span<const char> value) noexcept;
    common::Status check_arguments() noexcept;
    common::Status check_count(std::size_t most) noexcept;

    Options* options_;
    Line* error_;
    Source source_;
    const OptionSpec* pending_{nullptr};  ///< An option whose value is the next argument.
    bool options_ended_{false};           ///< After "--", every argument is a word.
    bool command_seen_{false};
    bool option_seen_{false};
    bool help_{false};
    bool version_{false};
};

common::Status Parser::take(const common::Span<const char> argument) noexcept {
    common::Status status{common::Status::kOk};
    if (pending_ != nullptr) {
        const OptionSpec& spec{*pending_};
        pending_ = nullptr;
        status = apply(spec, argument);
    } else if ((!options_ended_) && common::equals(argument, "--")) {
        options_ended_ = true;
    } else if ((!options_ended_) && (argument.size() > 1U) && (argument[0U] == '-')) {
        status = take_option(argument);
    } else {
        status = take_word(argument);
    }
    return status;
}

common::Status Parser::take_option(const common::Span<const char> argument) noexcept {
    // "--name=value" carries its value; otherwise the value is the next argument.
    const common::Result<std::size_t> equals_sign{common::find(argument, "=")};
    const common::Span<const char> name{equals_sign.has_value() ? argument.first(equals_sign.value()) : argument};
    const OptionSpec* spec{nullptr};
    for (std::size_t index{0U}; (spec == nullptr) && (index < kOptionSpecs.size()); ++index) {
        const OptionSpec& candidate{kOptionSpecs[index]};
        const bool short_match{(!candidate.short_name.empty()) && common::equals(name, candidate.short_name)};
        if (short_match || common::equals(name, candidate.long_name)) {
            spec = &candidate;
        }
    }
    option_seen_ = true;
    common::Status status{common::Status::kOk};
    if (spec == nullptr) {
        status = fail(*error_, "unknown option", name);
    } else if ((source_ == Source::kShell) && spec->connection) {
        error_->character('\'').text(name).text("' is a setting in the shell: use 'set ").text(spec->setting);
        error_->character('\'');
        status = common::Status::kInvalidArgument;
    } else if (equals_sign.has_value()) {
        status = spec->takes_value ? apply(*spec, argument.subspan(equals_sign.value() + 1U))
                                   : fail(*error_, "option takes no value", name);
    } else if (spec->takes_value) {
        pending_ = spec;
    } else {
        status = apply(*spec, common::Span<const char>{});
    }
    return status;
}

common::Status Parser::take_word(const common::Span<const char> word) noexcept {
    common::Status status{common::Status::kOk};
    if (command_seen_) {
        status = options_->arguments.push_back(word) ? common::Status::kOk : fail(*error_, "too many arguments", word);
    } else {
        const CommandSpec* spec{nullptr};
        for (std::size_t index{0U}; (spec == nullptr) && (index < kCommandSpecs.size()); ++index) {
            if (common::equals(word, kCommandSpecs[index].name)) {
                spec = &kCommandSpecs[index];
            }
        }
        if (spec == nullptr) {
            status = fail(*error_, "unknown command", word);
        } else if (spec->shell_only && (source_ == Source::kCommandLine)) {
            error_->character('\'').text(word).text("' works in the shell only (start elm327ctl without a command)");
            status = common::Status::kInvalidArgument;
        } else {
            options_->command = spec->command;
            command_seen_ = true;
        }
    }
    return status;
}

common::Status Parser::apply(const OptionSpec& spec, const common::Span<const char> value) noexcept {
    common::Status status{common::Status::kOk};
    if (spec.id == OptionId::kVerbose) {
        options_->verbose = true;
    } else if (spec.id == OptionId::kReadOnly) {
        options_->read_only = true;
    } else if (spec.id == OptionId::kYes) {
        options_->yes = true;
    } else if (spec.id == OptionId::kHelp) {
        help_ = true;
    } else if (spec.id == OptionId::kVersion) {
        version_ = true;
    } else {
        status = apply_value(spec, value, *options_, *error_);
    }
    return status;
}

common::Status Parser::finish() noexcept {
    common::Status status{common::Status::kOk};
    if (pending_ != nullptr) {
        status = fail(*error_, "option needs a value", span_of(pending_->long_name));
    } else if (help_) {
        options_->command = Command::kHelp;
    } else if (version_) {
        options_->command = Command::kVersion;
    } else if (command_seen_) {
        status = check_arguments();
    } else if (source_ == Source::kCommandLine) {
        options_->command = Command::kShell;
    } else if (option_seen_) {
        status = fail(*error_, "missing command", common::Span<const char>{});
    } else {
        options_->command = Command::kNone;
    }
    return status;
}

common::Status Parser::check_arguments() noexcept {
    common::Status status{common::Status::kOk};
    const bool none{options_->arguments.empty()};
    switch (options_->command) {
        case Command::kInfo:
        case Command::kPids:
        case Command::kDtc:
        case Command::kVin:
        case Command::kClear:
        case Command::kDisconnect:
        case Command::kShow:
        case Command::kQuit:
            status = check_count(0U);
            break;
        case Command::kCoding:
            status = check_count(1U);  // Optional: a make to filter by (vag, renault, bmw).
            break;
        case Command::kUds:
            if (none) {
                status = fail(*error_, "missing uds subcommand, e.g. 'read F190' (try 'uds help')",
                              common::Span<const char>{});
            }
            break;
        case Command::kRead:
        case Command::kMonitor:
            if (none) {
                status = fail(*error_, "missing PID, e.g. 0C", common::Span<const char>{});
            }
            for (std::size_t index{0U}; is_ok(status) && (index < options_->arguments.size()); ++index) {
                if (!parse_pid(options_->arguments[index]).has_value()) {
                    status = fail(*error_, "invalid PID (1 or 2 hexadecimal digits)", options_->arguments[index]);
                }
            }
            break;
        case Command::kRaw:
            if (none) {
                status = fail(*error_, "missing command for the adapter, e.g. ATI", common::Span<const char>{});
            }
            for (std::size_t index{0U}; is_ok(status) && (index < options_->arguments.size()); ++index) {
                if (options_->arguments[index].size() > Adapter::kMaxCommandLength) {
                    status = fail(*error_, "command too long for the adapter", options_->arguments[index]);
                }
            }
            break;
        case Command::kConnect:
            status = check_count(1U);
            if (is_ok(status) && (!none) &&
                (options_->arguments[0U].size() > ConnectionSettings::kMaxDeviceLength)) {
                status = fail(*error_, "device path too long", options_->arguments[0U]);
            }
            break;
        case Command::kSet:
            if (options_->arguments.size() == 1U) {
                status = fail(*error_, "missing value for the setting", options_->arguments[0U]);
            } else {
                status = check_count(2U);
            }
            break;
        case Command::kShell:
        case Command::kNone:
        case Command::kHelp:
        case Command::kVersion:
            break;
    }
    return status;
}

common::Status Parser::check_count(const std::size_t most) noexcept {
    return (options_->arguments.size() > most) ? fail(*error_, "unexpected argument", options_->arguments[most])
                                               : common::Status::kOk;
}

}  // namespace

bool needs_adapter(const Command command) noexcept {
    bool needed{false};
    switch (command) {
        case Command::kInfo:
        case Command::kPids:
        case Command::kRead:
        case Command::kMonitor:
        case Command::kDtc:
        case Command::kVin:
        case Command::kRaw:
        case Command::kClear:
            needed = true;
            break;
        case Command::kUds:     // Connects itself, so 'uds help' works without an adapter.
        case Command::kCoding:  // Prints a reference catalog; needs no adapter.
        case Command::kShell:
        case Command::kNone:
        case Command::kHelp:
        case Command::kVersion:
        case Command::kConnect:
        case Command::kDisconnect:
        case Command::kSet:
        case Command::kShow:
        case Command::kQuit:
            break;
    }
    return needed;
}

common::Status parse_options(const common::Span<const char* const> args, Options& options, Line& error) noexcept {
    options = Options{};
    Parser parser{options, error, Source::kCommandLine};
    common::Status status{common::Status::kOk};
    // args[0] is the program name.
    for (std::size_t index{1U}; is_ok(status) && (index < args.size()); ++index) {
        status = (args[index] != nullptr) ? parser.take(span_of(std::string_view{args[index]}))
                                          : common::Status::kInvalidArgument;
    }
    if (is_ok(status)) {
        status = parser.finish();
    }
    return status;
}

common::Status split_words(const common::Span<const char> line, Words& words, Line& error) noexcept {
    words.clear();
    common::Status status{common::Status::kOk};
    std::size_t index{0U};
    bool comment{false};
    while (is_ok(status) && (!comment) && (index < line.size())) {
        if (common::is_space(line[index])) {
            ++index;
        } else if (line[index] == '#') {
            comment = true;
        } else {
            const std::size_t start{index};
            while ((index < line.size()) && (!common::is_space(line[index]))) {
                ++index;
            }
            const common::Span<const char> word{line.subspan(start, index - start)};
            if (!words.push_back(word)) {
                status = fail(error, "too many words", common::Span<const char>{});
            }
        }
    }
    return status;
}

common::Status parse_line(const Words& words, const Options& settings, Options& options, Line& error) noexcept {
    options = settings;
    options.command = Command::kNone;
    options.arguments.clear();
    Parser parser{options, error, Source::kShell};
    common::Status status{common::Status::kOk};
    for (std::size_t index{0U}; is_ok(status) && (index < words.size()); ++index) {
        status = parser.take(words[index]);
    }
    if (is_ok(status)) {
        status = parser.finish();
    }
    return status;
}

common::Status apply_setting(const common::Span<const char> name, const common::Span<const char> value,
                             Options& options, Line& error) noexcept {
    const OptionSpec* spec{nullptr};
    for (std::size_t index{0U}; (spec == nullptr) && (index < kOptionSpecs.size()); ++index) {
        const std::string_view setting{kOptionSpecs[index].setting};
        if ((!setting.empty()) && common::equals(name, setting)) {
            spec = &kOptionSpecs[index];
        }
    }
    common::Status status{common::Status::kOk};
    if (spec == nullptr) {
        status = fail(error, "unknown setting (device, baud, protocol, timeout, interval, count, verbose)", name);
    } else if (spec->id == OptionId::kVerbose) {
        if (is_one_of(value, kOnWords)) {
            options.verbose = true;
        } else if (is_one_of(value, kOffWords)) {
            options.verbose = false;
        } else {
            status = fail(error, "invalid value for verbose (on or off)", value);
        }
    } else if (spec->id == OptionId::kReadOnly) {
        if (is_one_of(value, kOnWords)) {
            options.read_only = true;
        } else if (is_one_of(value, kOffWords)) {
            options.read_only = false;
        } else {
            status = fail(error, "invalid value for readonly (on or off)", value);
        }
    } else {
        status = apply_value(*spec, value, options, error);
    }
    return status;
}

bool is_connection_setting(const common::Span<const char> name) noexcept {
    bool connection{false};
    for (const OptionSpec& spec : kOptionSpecs) {
        connection = connection || (spec.connection && common::equals(name, spec.setting));
    }
    return connection;
}

common::Span<const std::string_view> shell_commands() noexcept {
    return common::Span<const std::string_view>{kCommandNames.data(), kCommandNames.size()};
}

common::Span<const std::string_view> setting_names() noexcept {
    return common::Span<const std::string_view>{kSettingNames.data(), kSettingNames.size()};
}

common::Result<std::uint8_t> parse_pid(const common::Span<const char> text) noexcept {
    return ((!text.empty()) && (text.size() <= 2U))
               ? common::parse_hex<std::uint8_t>(text)
               : common::Result<std::uint8_t>::failure(common::Status::kInvalidArgument);
}

void print_usage(Console& console) noexcept {
    for (const std::string_view line : kUsage) {
        console.print(line);
    }
}

void print_shell_help(Console& console) noexcept {
    for (const std::string_view line : kShellHelp) {
        console.print(line);
    }
}

}  // namespace cli
}  // namespace elm327
