/// @file commands.hpp
/// @brief The commands of elm327ctl, and the session that runs them over a connection.

#ifndef ELM327_CLI_COMMANDS_HPP
#define ELM327_CLI_COMMANDS_HPP

#include <cstdint>

#include "cli/console.hpp"
#include "cli/options.hpp"
#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/controller/controller.hpp"

namespace elm327 {
namespace cli {

inline constexpr std::int32_t kExitSuccess{0};
/// The device, the adapter or the vehicle failed, or a value could not be read.
inline constexpr std::int32_t kExitFailure{1};
/// The command line is invalid.
inline constexpr std::int32_t kExitUsage{2};
/// A stop signal ended the command: 128 + SIGINT. main() reports the signal that actually arrived.
inline constexpr std::int32_t kExitInterrupted{130};

/// How the messages of a Session refer to the settings: with the options of the command line
/// ("-d"), or with the 'set' command of the shell.
enum class Style : std::uint8_t {
    kCommandLine,
    kShell,
};

/// Runs commands over one connection to the adapter. The command-line tool runs one command and
/// closes the connection; the shell keeps it open from one command to the next.
///
/// Results go to the out Console; errors, the notices of the shell and the traffic with --verbose
/// go to the err Console. Once the cancel descriptor is readable (see StopSignals), a command
/// stops at the next opportunity.
///
/// The object embeds a Controller (about 25 KiB) and never allocates. Not thread-safe.
class Session final {
public:
    Session(Console& out, Console& err, std::int32_t cancel_descriptor, Style style) noexcept;

    /// Runs the command of @p options: one for which needs_adapter() is true, kConnect or
    /// kDisconnect. A command that needs the adapter connects to it first, with
    /// options.connection, unless the session is connected.
    /// @return the exit code.
    [[nodiscard]] std::int32_t run(const Options& options) noexcept;

    /// Closes the connection, if there is one.
    void disconnect() noexcept {
        controller_.disconnect();
    }

    [[nodiscard]] const Controller& controller() const noexcept {
        return controller_;
    }

private:
    [[nodiscard]] std::int32_t connect(Console& notices) noexcept;
    [[nodiscard]] std::int32_t dispatch() noexcept;
    [[nodiscard]] std::int32_t run_info() noexcept;
    [[nodiscard]] std::int32_t run_pids() noexcept;
    [[nodiscard]] std::int32_t run_read() noexcept;
    [[nodiscard]] std::int32_t run_monitor() noexcept;
    [[nodiscard]] std::int32_t run_dtc() noexcept;
    [[nodiscard]] std::int32_t run_vin() noexcept;
    [[nodiscard]] std::int32_t run_raw() noexcept;
    [[nodiscard]] std::int32_t run_clear() noexcept;
    [[nodiscard]] std::int32_t run_uds() noexcept;
    [[nodiscard]] std::int32_t run_uds_write() noexcept;
    [[nodiscard]] std::int32_t run_uds_reset() noexcept;
    [[nodiscard]] std::int32_t run_uds_routine() noexcept;
    [[nodiscard]] std::int32_t run_uds_key() noexcept;
    [[nodiscard]] std::int32_t run_uds_send() noexcept;
    [[nodiscard]] std::int32_t run_coding() noexcept;
    [[nodiscard]] std::int32_t run_disconnect() noexcept;

    /// Sends the built UDS request @p command, then prints the decoded reply (a positive response
    /// and its data, or the named negative response code) under @p label. @return the exit code.
    [[nodiscard]] std::int32_t send_uds(std::string_view label, std::string_view command) noexcept;

    /// Prints "elm327ctl: <what>: <reason>", see explain_failure(). @return the exit code for @p status.
    std::int32_t fail(std::string_view what, common::Status status, const Response& response) noexcept;

    /// Explains why connect() failed. @return the exit code for @p status.
    std::int32_t report_connect_failure(common::Status status) noexcept;

    /// Prints the reply to the AT command @p command, a line of text such as "12.6V", under
    /// @p label; "unknown" if the adapter does not know the command. @return the status of the query.
    common::Status print_query(std::string_view label, std::string_view command) noexcept;

    void print_field(std::string_view label, common::Span<const char> value) noexcept;

    Console* out_;
    Console* err_;
    std::int32_t cancel_descriptor_;
    Style style_;
    const Options* options_{nullptr};  ///< The options of the running command.
    Controller controller_{};
    Response response_{};  ///< Replies to raw commands.
};

/// Runs the command line @p args (args[0] is the program name): parses it and runs its command
/// over a connection that is closed again afterwards; without a command, runs the interactive
/// shell, which reads its commands from @p input (see run_shell()). Results go to @p out; errors,
/// and the traffic with --verbose, go to @p err. Once @p cancel_descriptor is readable (see
/// StopSignals; -1 for none), the command stops at the next opportunity and the port is closed.
/// @return the exit code.
[[nodiscard]] std::int32_t run(common::Span<const char* const> args, std::int32_t input, Console& out, Console& err,
                               std::int32_t cancel_descriptor) noexcept;

}  // namespace cli
}  // namespace elm327

#endif  // ELM327_CLI_COMMANDS_HPP
