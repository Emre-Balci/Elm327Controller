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
/// No signal handler is involved, so nothing has to be async-signal-safe. macOS and iOS have no
/// signalfd: there StopSignals installs a handler for the stop signals, which writes the number of
/// each into a pipe, the descriptor, and puts the earlier handlers back when it closes.
///
/// Windows has no such signals, but console control events, which a handler function receives on a
/// thread of its own. There StopSignals installs that handler, which records the event and signals a
/// manual-reset event, the descriptor: Ctrl+C is reported as SIGINT, Ctrl+Break as SIGBREAK, and
/// closing the console window (or logging off, or shutting down) as SIGTERM. Windows ends the
/// process a few seconds after a close event at the latest.

#ifndef ELM327_CONTROLLER_SIGNALS_HPP
#define ELM327_CONTROLLER_SIGNALS_HPP

#include <cstdint>

#include "common/core/config.hpp"
#include "common/core/status.hpp"

namespace elm327 {

/// SIGINT (Ctrl+C), SIGTERM, SIGHUP (closed terminal) and SIGPIPE (closed output pipe), received
/// through a descriptor; on Windows, the console control events (see above). A program needs one,
/// created at the start of main().
class StopSignals final {
public:
    StopSignals() noexcept = default;

    /// Closes the descriptor. The signals stay blocked. On Windows the handler is removed, so that
    /// Ctrl+C ends the process again.
    ~StopSignals();

    StopSignals(const StopSignals&) = delete;
    StopSignals& operator=(const StopSignals&) = delete;
    StopSignals(StopSignals&&) = delete;
    StopSignals& operator=(StopSignals&&) = delete;

    /// Blocks the signals for the calling thread and opens the descriptor. Call it before any other
    /// thread starts, so that all threads inherit the blocked signals.
    /// @return kOk; kInvalidArgument if it is open already (on Windows, macOS and iOS, also when
    ///         another StopSignals is open, since a process has one set of handlers); kSystemError if
    ///         a call failed.
    common::Status open() noexcept;

    /// The descriptor, readable once a signal has arrived (on Windows, the event, signaled then);
    /// kInvalidNativeHandle (-1 on Linux) when not open.
    [[nodiscard]] common::NativeHandle descriptor() const noexcept {
        return descriptor_;
    }

    /// @return the number of the first signal that arrived, e.g. SIGINT, or 0 if none did. Does not wait.
    [[nodiscard]] std::int32_t received() noexcept;

private:
    common::NativeHandle descriptor_{common::kInvalidNativeHandle};
    std::int32_t signal_{0};
};

/// Takes the stop requests pending on @p descriptor, the descriptor of a StopSignals or an Event (as
/// the tests and the GUI use), so that it is no longer readable. Does not wait.
/// @return the signal that was pending: one that ends a program (see ends_program()) when one of
///         them was; otherwise SIGINT for a SIGINT or an Event; 0 if nothing was pending.
[[nodiscard]] std::int32_t take_stop_request(common::NativeHandle descriptor) noexcept;

/// @return true for the signals after which an interactive program ends instead of going on:
///         every stop signal except SIGINT, i.e. SIGTERM, SIGHUP and SIGPIPE (on Windows, SIGTERM
///         and SIGBREAK).
[[nodiscard]] bool ends_program(std::int32_t signal) noexcept;

}  // namespace elm327

#endif  // ELM327_CONTROLLER_SIGNALS_HPP
