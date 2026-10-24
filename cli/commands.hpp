/// @file commands.hpp
/// @brief The commands of elm327ctl.

#ifndef ELM327_CLI_COMMANDS_HPP
#define ELM327_CLI_COMMANDS_HPP

#include <cstdint>

#include "cli/console.hpp"
#include "common/containers/span.hpp"

namespace elm327 {
namespace cli {

inline constexpr std::int32_t kExitSuccess{0};
/// The device, the adapter or the vehicle failed, or a value could not be read.
inline constexpr std::int32_t kExitFailure{1};
/// The command line is invalid.
inline constexpr std::int32_t kExitUsage{2};
/// A stop signal ended the command: 128 + SIGINT. main() reports the signal that actually arrived.
inline constexpr std::int32_t kExitInterrupted{130};

/// Runs the command line @p args (args[0] is the program name): parses it, opens and initializes
/// the adapter, runs the command and closes the adapter again. Results go to @p out; errors, and
/// the traffic with --verbose, go to @p err. Once @p cancel_descriptor is readable (see
/// StopSignals; -1 for none), the command stops at the next opportunity and the port is closed.
/// @return the exit code.
[[nodiscard]] std::int32_t run(common::Span<const char* const> args, Console& out, Console& err,
                               std::int32_t cancel_descriptor) noexcept;

}  // namespace cli
}  // namespace elm327

#endif  // ELM327_CLI_COMMANDS_HPP
