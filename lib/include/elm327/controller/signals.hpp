/// @file signals.hpp
/// @brief Stopping a program cleanly on Ctrl+C.
///
/// A process killed by a signal leaves its serial port to the kernel. The kernel closes it, but it
/// cannot always release the exclusive access the port was opened with (TIOCEXCL): a device that
/// stays in use elsewhere, such as the pseudo-terminal of the simulator, then refuses every later
/// open. StopSignals therefore blocks the stop signals and delivers them through a signalfd: the
/// descriptor becomes readable, every Adapter operation that watches it ends with kInterrupted,
/// and the program closes the port and exits on its own.
///
/// An interactive program goes on after Ctrl+C instead: take_stop_request() makes the descriptor
/// unreadable again once the interrupted command has ended.
///
/// No signal handler is involved, so nothing has to be async-signal-safe.

#ifndef ELM327_CONTROLLER_SIGNALS_HPP
#define ELM327_CONTROLLER_SIGNALS_HPP

#include <cstdint>

#include "common/core/status.hpp"

namespace elm327 {

/// SIGINT (Ctrl+C), SIGTERM, SIGHUP (closed terminal) and SIGPIPE (closed output pipe), received
/// through a descriptor. A program needs one, created at the start of main().
class StopSignals final {
public:
    StopSignals() noexcept = default;

    /// Closes the descriptor. The signals stay blocked.
    ~StopSignals();

    StopSignals(const StopSignals&) = delete;
    StopSignals& operator=(const StopSignals&) = delete;
    StopSignals(StopSignals&&) = delete;
    StopSignals& operator=(StopSignals&&) = delete;

    /// Blocks the signals for the calling thread and opens the descriptor. Call it before any other
    /// thread starts, so that all threads inherit the blocked signals.
    /// @return kOk; kInvalidArgument if it is open already; kSystemError if a call failed.
    common::Status open() noexcept;

    /// The descriptor, readable once a signal has arrived; -1 when not open.
    [[nodiscard]] std::int32_t descriptor() const noexcept {
        return descriptor_;
    }

    /// @return the number of the first signal that arrived, e.g. SIGINT, or 0 if none did. Does not wait.
    [[nodiscard]] std::int32_t received() noexcept;

private:
    std::int32_t descriptor_{-1};
    std::int32_t signal_{0};
};

/// Takes the stop requests pending on @p descriptor, the descriptor of a StopSignals or an eventfd
/// (as the tests and the GUI use), so that it is no longer readable. Does not wait.
/// @return the signal that was pending: SIGTERM, SIGHUP or SIGPIPE when one of them was, since they
///         end a program; otherwise SIGINT for a SIGINT or an eventfd; 0 if nothing was pending.
[[nodiscard]] std::int32_t take_stop_request(std::int32_t descriptor) noexcept;

/// @return true for the signals after which an interactive program ends instead of going on:
///         every stop signal except SIGINT.
[[nodiscard]] bool ends_program(std::int32_t signal) noexcept;

}  // namespace elm327

#endif  // ELM327_CONTROLLER_SIGNALS_HPP
