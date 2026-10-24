/// @file main.cpp
/// @brief elm327ctl: command-line controller for ELM327 OBD-II adapters. See print_usage().

#include <unistd.h>

#include <cstddef>
#include <cstdint>

#include "cli/commands.hpp"
#include "cli/console.hpp"
#include "cli/signals.hpp"
#include "common/containers/span.hpp"

int main(const int argc, const char* const argv[]) {
    elm327::cli::Console out{STDOUT_FILENO};
    elm327::cli::Console err{STDERR_FILENO};
    elm327::cli::StopSignals stop{};
    // Without the descriptor, Ctrl+C still ends the program, only without closing the port first.
    static_cast<void>(stop.open());
    const common::Span<const char* const> args{argv, static_cast<std::size_t>(argc)};
    std::int32_t exit_code{elm327::cli::run(args, out, err, stop.descriptor())};
    const std::int32_t signal{stop.received()};
    if (signal != 0) {
        exit_code = 128 + signal;  // The shell's convention for a program ended by a signal.
    }
    return exit_code;
}
