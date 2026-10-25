/// @file options.hpp
/// @brief The command language of elm327ctl: the command line, and the lines typed into the shell.
///
/// Both use the same words: a command with its arguments, and options that may come before or
/// after the command, e.g. "-i 250 monitor 0C". On the command line the options set up the whole
/// run. In the shell the connection settings are changed with 'set' instead, and options given on
/// a line apply to the command of that line only.

#ifndef ELM327_CLI_OPTIONS_HPP
#define ELM327_CLI_OPTIONS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cli/console.hpp"
#include "common/containers/span.hpp"
#include "common/containers/static_vector.hpp"
#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "elm327/controller/controller.hpp"

namespace elm327 {
namespace cli {

enum class Command : std::uint8_t {
    kShell,  ///< No command on the command line: run the interactive shell.
    kNone,   ///< An empty line in the shell, or one with only a comment.
    kHelp,
    kVersion,
    kInfo,
    kPids,
    kRead,
    kMonitor,
    kDtc,
    kVin,
    kRaw,
    kClear,
    kUds,
    kCoding,
    // The commands below exist in the shell only.
    kConnect,
    kDisconnect,
    kSet,
    kShow,
    kQuit,
};

/// @return true for the commands that talk to the adapter, info to raw.
[[nodiscard]] bool needs_adapter(Command command) noexcept;

/// The parsed command line, or a parsed line of the shell.
struct Options final {
    static constexpr std::size_t kMaxArguments{32U};

    Command command{Command::kShell};
    /// Device, bit rate, protocol, and the reply timeout of a command (timeouts.command).
    ConnectionSettings connection{};
    /// monitor: time between samples.
    common::Duration interval{std::chrono::seconds{1}};
    /// monitor: number of samples; 0 runs until Ctrl+C.
    std::uint32_t count{0U};
    /// Print the traffic with the adapter.
    bool verbose{false};
    /// Read-only mode: refuse any command that could change the vehicle (see Controller).
    bool read_only{false};
    /// Confirm a write (clear, uds write) without an interactive prompt.
    bool yes{false};
    /// The arguments of the command, e.g. the PIDs of read. They view the parsed text, which must
    /// outlive the options.
    common::StaticVector<common::Span<const char>, kMaxArguments> arguments{};
};

/// The words of a line typed into the shell.
using Words = common::StaticVector<common::Span<const char>, Options::kMaxArguments + 8U>;

/// Parses the command line @p args; args[0] is the program name. Options may come before or after
/// the command; "--" ends the options. Without a command (and without --help or --version), the
/// result is Command::kShell. The shell's own commands are rejected.
/// @return kOk; kInvalidArgument for a usage error, with the reason in @p error.
[[nodiscard]] common::Status parse_options(common::Span<const char* const> args, Options& options,
                                           Line& error) noexcept;

/// Splits @p line, typed into the shell, into @p words at spaces and tabs. A word that starts with
/// '#' starts a comment, which ends the line.
/// @return kOk; kInvalidArgument with the reason in @p error if the line has too many words.
[[nodiscard]] common::Status split_words(common::Span<const char> line, Words& words, Line& error) noexcept;

/// Parses a line of the shell, split into @p words, into @p options. They start as a copy of
/// @p settings, the shell's settings, so options on the line apply to its command only. The
/// connection options (-d, -b, -p, -t) are settings in the shell and are rejected on a line. A
/// line without words gives Command::kNone.
/// @return kOk; kInvalidArgument for a usage error, with the reason in @p error.
[[nodiscard]] common::Status parse_line(const Words& words, const Options& settings, Options& options,
                                        Line& error) noexcept;

/// Changes the setting @p name to @p value in @p options, as 'set NAME VALUE' does: device, baud,
/// protocol, timeout, interval, count (the values of the options of the same names) or verbose
/// ("on" or "off").
/// @return kOk; kInvalidArgument with the reason in @p error.
[[nodiscard]] common::Status apply_setting(common::Span<const char> name, common::Span<const char> value,
                                           Options& options, Line& error) noexcept;

/// @return true if the setting @p name belongs to the connection (device, baud, protocol,
///         timeout): a change applies when the shell connects the next time.
[[nodiscard]] bool is_connection_setting(common::Span<const char> name) noexcept;

/// The commands of the shell, and the names of its settings, e.g. for completion.
[[nodiscard]] common::Span<const std::string_view> shell_commands() noexcept;
[[nodiscard]] common::Span<const std::string_view> setting_names() noexcept;

/// Parses a PID: one or two hexadecimal digits, e.g. "0C" or "c".
[[nodiscard]] common::Result<std::uint8_t> parse_pid(common::Span<const char> text) noexcept;

/// Writes the usage text of the command line.
void print_usage(Console& console) noexcept;

/// Writes the help of the shell.
void print_shell_help(Console& console) noexcept;

}  // namespace cli
}  // namespace elm327

#endif  // ELM327_CLI_OPTIONS_HPP
