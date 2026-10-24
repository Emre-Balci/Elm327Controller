/// @file signals.hpp
/// @brief Stopping elm327ctl cleanly on Ctrl+C.
///
/// A process killed by a signal leaves its serial port to the kernel. The kernel closes it, but it
/// cannot always release the exclusive access the port was opened with (TIOCEXCL): a device that
/// stays in use elsewhere, such as the pseudo-terminal of the simulator, then refuses every later
/// open. StopSignals therefore blocks the stop signals and delivers them through a signalfd: the
/// descriptor becomes readable, every Adapter operation that watches it ends with kInterrupted,
/// and the program closes the port and exits on its own.
///
/// No signal handler is involved, so nothing has to be async-signal-safe.

#ifndef ELM327_CLI_SIGNALS_HPP
#define ELM327_CLI_SIGNALS_HPP

#include <cstdint>

#include "common/core/status.hpp"
#include "common/time/deadline.hpp"

namespace elm327 {
namespace cli {

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

/// Waits until @p deadline, or until @p descriptor becomes readable; -1 waits for the deadline only.
/// @return kOk when the deadline passed; kInterrupted when the descriptor became readable first;
///         kSystemError if waiting failed.
common::Status wait_until(std::int32_t descriptor, const common::Deadline& deadline) noexcept;

}  // namespace cli
}  // namespace elm327

#endif  // ELM327_CLI_SIGNALS_HPP
