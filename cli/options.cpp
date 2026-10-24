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
    kHelp,
    kVersion,
};

struct OptionSpec final {
    OptionId id;
    std::string_view short_name;  ///< Empty if the option has none.
    std::string_view long_name;
    bool takes_value;
};

constexpr std::array<OptionSpec, 9U> kOptionSpecs{{
    {OptionId::kDevice, "-d", "--device", true},
    {OptionId::kBaud, "-b", "--baud", true},
    {OptionId::kProtocol, "-p", "--protocol", true},
    {OptionId::kTimeout, "-t", "--timeout", true},
    {OptionId::kInterval, "-i", "--interval", true},
    {OptionId::kCount, "-n", "--count", true},
    {OptionId::kVerbose, "-v", "--verbose", false},
    {OptionId::kHelp, "-h", "--help", false},
    {OptionId::kVersion, "", "--version", false},
}};

struct CommandSpec final {
    Command command;
    std::string_view name;
};

constexpr std::array<CommandSpec, 8U> kCommandSpecs{{
    {Command::kInfo, "info"},
    {Command::kPids, "pids"},
    {Command::kRead, "read"},
    {Command::kMonitor, "monitor"},
    {Command::kDtc, "dtc"},
    {Command::kVin, "vin"},
    {Command::kRaw, "raw"},
    {Command::kHelp, "help"},
}};

/// The protocol numbers of the ELM327, and the same in lower case at the same positions.
constexpr std::string_view kProtocols{"0123456789ABC"};
constexpr std::string_view kLowerProtocols{"0123456789abc"};

constexpr std::array<std::string_view, 24U> kUsage{{
    "Usage: elm327ctl [options] <command> [arguments]",
    "",
    "Talks to an ELM327 OBD-II adapter on a serial port.",
    "",
    "Commands:",
    "  info              adapter identity, supply voltage and the vehicle's protocol",
    "  pids              list the service 01 PIDs the vehicle supports",
    "  read PID...       read service 01 PIDs once, e.g. 'read 0C 0D 05'",
    "  monitor PID...    read PIDs repeatedly until Ctrl+C (see -i, -n)",
    "  dtc               read the MIL status and the stored, pending and permanent DTCs",
    "  vin               read the vehicle identification number",
    "  raw COMMAND...    send AT commands or OBD requests and print the replies",
    "  help              show this help",
    "",
    "Options:",
    "  -d, --device PATH   serial device (default /dev/ttyUSB0)",
    "  -b, --baud RATE     bit rate (default 38400)",
    "  -p, --protocol N    OBD protocol 0-9 or A-C (default 0: automatic)",
    "  -t, --timeout MS    reply timeout of a command in ms (default 5000)",
    "  -i, --interval MS   monitor: time between samples in ms (default 1000)",
    "  -n, --count N       monitor: stop after N samples (default 0: until Ctrl+C)",
    "  -v, --verbose       print the traffic with the adapter on stderr",
    "  -h, --help          show this help",
    "      --version       show the version",
}};

// Too few lines do not compile; too many would leave empty lines at the end.
static_assert(!kUsage.back().empty(), "kUsage has more entries than lines");

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
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

/// Consumes the command line one argument at a time.
class Parser final {
public:
    Parser(Options& options, Line& error) noexcept : options_{&options}, error_{&error} {}

    common::Status take(common::Span<const char> argument) noexcept;
    common::Status finish() noexcept;

private:
    common::Status take_option(common::Span<const char> argument) noexcept;
    common::Status take_word(common::Span<const char> word) noexcept;
    common::Status apply(const OptionSpec& spec, common::Span<const char> value) noexcept;
    common::Status check_arguments() noexcept;
    common::Status fail(std::string_view reason, common::Span<const char> subject) noexcept;

    Options* options_;
    Line* error_;
    const OptionSpec* pending_{nullptr};  ///< An option whose value is the next argument.
    bool options_ended_{false};           ///< After "--", every argument is a word.
    bool command_seen_{false};
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
    common::Status status{common::Status::kOk};
    if (spec == nullptr) {
        status = fail("unknown option", name);
    } else if (equals_sign.has_value()) {
        status = spec->takes_value ? apply(*spec, argument.subspan(equals_sign.value() + 1U))
                                   : fail("option takes no value", name);
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
        status = options_->arguments.push_back(word) ? common::Status::kOk : fail("too many arguments", word);
    } else {
        bool found{false};
        for (std::size_t index{0U}; (!found) && (index < kCommandSpecs.size()); ++index) {
            found = common::equals(word, kCommandSpecs[index].name);
            if (found) {
                options_->command = kCommandSpecs[index].command;
            }
        }
        command_seen_ = found;
        if (!found) {
            status = fail("unknown command", word);
        }
    }
    return status;
}

common::Status Parser::apply(const OptionSpec& spec, const common::Span<const char> value) noexcept {
    common::Status status{common::Status::kOk};
    switch (spec.id) {
        case OptionId::kDevice:
            // The value runs to the end of its command-line argument, so it is NUL-terminated.
            options_->device = value.data();
            if (value.empty()) {
                status = fail("empty device path", span_of(spec.long_name));
            }
            break;
        case OptionId::kBaud: {
            const common::Result<std::uint32_t> rate{common::parse_unsigned<std::uint32_t>(value)};
            const common::Result<common::BaudRate> baud_rate{
                rate.has_value() ? common::baud_rate_from_bits_per_second(rate.value())
                                 : common::Result<common::BaudRate>::failure(common::Status::kInvalidArgument)};
            options_->baud_rate = baud_rate.value_or(common::BaudRate::k38400);
            if (!baud_rate.has_value()) {
                status = fail("unsupported baud rate", value);
            }
            break;
        }
        case OptionId::kProtocol: {
            std::size_t position{std::string_view::npos};
            if (value.size() == 1U) {
                position = kProtocols.find(value[0U]);
                if (position == std::string_view::npos) {
                    position = kLowerProtocols.find(value[0U]);
                }
            }
            if (position != std::string_view::npos) {
                options_->protocol = kProtocols[position];
            } else {
                status = fail("invalid protocol (0-9 or A-C)", value);
            }
            break;
        }
        case OptionId::kTimeout:
            status = parse_milliseconds(value, options_->timeout);
            if (!is_ok(status)) {
                status = fail("invalid timeout in ms", value);
            }
            break;
        case OptionId::kInterval:
            status = parse_milliseconds(value, options_->interval);
            if (!is_ok(status)) {
                status = fail("invalid interval in ms", value);
            }
            break;
        case OptionId::kCount: {
            const common::Result<std::uint32_t> count{common::parse_unsigned<std::uint32_t>(value)};
            options_->count = count.value_or(0U);
            if (!count.has_value()) {
                status = fail("invalid count", value);
            }
            break;
        }
        case OptionId::kVerbose:
            options_->verbose = true;
            break;
        case OptionId::kHelp:
            help_ = true;
            break;
        case OptionId::kVersion:
            version_ = true;
            break;
    }
    return status;
}

common::Status Parser::finish() noexcept {
    common::Status status{common::Status::kOk};
    if (pending_ != nullptr) {
        status = fail("option needs a value", span_of(pending_->long_name));
    } else if (help_) {
        options_->command = Command::kHelp;
    } else if (version_) {
        options_->command = Command::kVersion;
    } else if (!command_seen_) {
        status = fail("missing command", common::Span<const char>{});
    } else {
        status = check_arguments();
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
            if (!none) {
                status = fail("unexpected argument", options_->arguments[0U]);
            }
            break;
        case Command::kRead:
        case Command::kMonitor:
            if (none) {
                status = fail("missing PID, e.g. 0C", common::Span<const char>{});
            }
            for (std::size_t index{0U}; is_ok(status) && (index < options_->arguments.size()); ++index) {
                if (!parse_pid(options_->arguments[index]).has_value()) {
                    status = fail("invalid PID (1 or 2 hexadecimal digits)", options_->arguments[index]);
                }
            }
            break;
        case Command::kRaw:
            if (none) {
                status = fail("missing command for the adapter, e.g. ATI", common::Span<const char>{});
            }
            for (std::size_t index{0U}; is_ok(status) && (index < options_->arguments.size()); ++index) {
                if (options_->arguments[index].size() > Adapter::kMaxCommandLength) {
                    status = fail("command too long for the adapter", options_->arguments[index]);
                }
            }
            break;
        case Command::kHelp:
        case Command::kVersion:
            break;
    }
    return status;
}

common::Status Parser::fail(const std::string_view reason, const common::Span<const char> subject) noexcept {
    error_->text(reason);
    if (!subject.empty()) {
        error_->text(": '").text(subject).character('\'');
    }
    return common::Status::kInvalidArgument;
}

}  // namespace

common::Status parse_options(const common::Span<const char* const> args, Options& options, Line& error) noexcept {
    options = Options{};
    Parser parser{options, error};
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

}  // namespace cli
}  // namespace elm327
