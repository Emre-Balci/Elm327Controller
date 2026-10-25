/// @file shell.hpp
/// @brief The interactive shell of elm327ctl.
///
/// Started without a command, elm327ctl reads commands line by line, from a terminal with line
/// editing, history and completion, or from a pipe or a file without them. The connection to the
/// adapter stays open from one command to the next: the first command that needs the adapter
/// connects, and 'set' changes the settings of the next connection.
///
/// Ctrl+C stops the command that runs and returns to the prompt; at the prompt it discards the
/// line. SIGTERM, SIGHUP and SIGPIPE end the shell (on Windows, Ctrl+Break and closing the console
/// window), as does 'quit', Ctrl+D or the end of the input.

#ifndef ELM327_CLI_SHELL_HPP
#define ELM327_CLI_SHELL_HPP

#include <cstdint>

#include "cli/console.hpp"
#include "cli/options.hpp"
#include "common/core/config.hpp"

namespace elm327 {
namespace cli {

/// Runs the shell with the settings of @p options (the options of the command line), reading its
/// commands from @p input. Results go to @p out, errors and notices to @p err. @p cancel_descriptor
/// is the descriptor of a StopSignals, or an eventfd (on Windows, a manual-reset event;
/// kInvalidNativeHandle for none): once it is readable, the running command stops, and the shell
/// takes the request and goes on.
/// @return the exit code of the last command (0 if there was none); 128 + the signal number when a
///         signal ended the shell.
[[nodiscard]] std::int32_t run_shell(const Options& options, common::NativeHandle input, Console& out, Console& err,
                                     common::NativeHandle cancel_descriptor) noexcept;

}  // namespace cli
}  // namespace elm327

#endif  // ELM327_CLI_SHELL_HPP
