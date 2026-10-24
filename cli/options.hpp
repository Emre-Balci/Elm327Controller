/// @file options.hpp
/// @brief The command line of elm327ctl.

#ifndef ELM327_CLI_OPTIONS_HPP
#define ELM327_CLI_OPTIONS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "cli/console.hpp"
#include "common/containers/span.hpp"
#include "common/containers/static_vector.hpp"
#include "common/core/status.hpp"
#include "common/file/serial_port.hpp"
#include "common/time/clock.hpp"

namespace elm327 {
namespace cli {

enum class Command : std::uint8_t {
    kHelp,
    kVersion,
    kInfo,
    kPids,
    kRead,
    kMonitor,
    kDtc,
    kVin,
    kRaw,
};

/// The first USB serial adapter.
inline constexpr const char* kDefaultDevice{"/dev/ttyUSB0"};

/// The parsed command line.
struct Options final {
    static constexpr std::size_t kMaxArguments{32U};

    Command command{Command::kHelp};
    /// Serial device; a NUL-terminated string from the command line.
    const char* device{kDefaultDevice};
    common::BaudRate baud_rate{common::BaudRate::k38400};
    /// ELM327 protocol number: '0' (automatic), '1' to '9', 'A' to 'C'.
    char protocol{'0'};
    /// Reply timeout of a command.
    common::Duration timeout{std::chrono::seconds{5}};
    /// monitor: time between samples.
    common::Duration interval{std::chrono::seconds{1}};
    /// monitor: number of samples; 0 runs until Ctrl+C.
    std::uint32_t count{0U};
    /// Print the traffic with the adapter.
    bool verbose{false};
    /// The arguments of the command, e.g. the PIDs of read.
    common::StaticVector<common::Span<const char>, kMaxArguments> arguments{};
};

/// Parses the command line @p args; args[0] is the program name. Options may come before or after
/// the command; "--" ends the options.
/// @return kOk; kInvalidArgument for a usage error, with the reason in @p error.
[[nodiscard]] common::Status parse_options(common::Span<const char* const> args, Options& options,
                                           Line& error) noexcept;

/// Parses a PID: one or two hexadecimal digits, e.g. "0C" or "c".
[[nodiscard]] common::Result<std::uint8_t> parse_pid(common::Span<const char> text) noexcept;

/// Writes the usage text.
void print_usage(Console& console) noexcept;

}  // namespace cli
}  // namespace elm327

#endif  // ELM327_CLI_OPTIONS_HPP
