#include "elm327/adapter/adapter.hpp"

#include <poll.h>
#include <time.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace elm327 {
namespace {

timespec to_relative_timespec(const common::Duration duration) noexcept {
    const std::int64_t ns{duration.count()};
    timespec ts{};
    ts.tv_sec = ns / common::MonotonicClock::kNanosPerSecond;
    ts.tv_nsec = ns % common::MonotonicClock::kNanosPerSecond;
    return ts;
}

/// Waits until @p port has input or reports an error, @p cancel is readable, or @p deadline passes.
/// @return kOk, kInterrupted or kTimeout.
common::Status wait_for_input(const common::NativeHandle port, const common::NativeHandle cancel,
                              const common::Deadline& deadline) noexcept {
    std::array<pollfd, 2U> entries{};
    entries[0U].fd = port;
    entries[0U].events = POLLIN;
    entries[1U].fd = cancel;  // poll() skips a negative descriptor.
    entries[1U].events = POLLIN;
    timespec timeout{};
    const timespec* timeout_pointer{nullptr};
    if (!deadline.is_never()) {
        timeout = to_relative_timespec(deadline.remaining());
        timeout_pointer = &timeout;
    }
    const int ready{::ppoll(entries.data(), entries.size(), timeout_pointer, nullptr)};
    common::Status status{common::Status::kTimeout};
    if (ready < 0) {
        // After a signal handler ran, the read finds no data and the caller waits again.
        status = (errno == EINTR) ? common::Status::kOk : common::Status::kSystemError;
    } else if (ready == 0) {
        status = common::Status::kTimeout;
    } else if (static_cast<std::uint32_t>(entries[1U].revents) != 0U) {
        status = common::Status::kInterrupted;
    } else {
        status = common::Status::kOk;  // Input, a hang-up or an error: the read tells which.
    }
    return status;
}

}  // namespace

common::Status Adapter::open_device(const char* const path, const common::SerialOptions& options) noexcept {
    return port_.open(path, options);
}

common::Status Adapter::close_device() noexcept {
    return port_.close();
}

common::Status Adapter::discard_input() noexcept {
    return port_.discard_input();
}

common::Status Adapter::write_all(const common::Span<const std::uint8_t> data,
                                  const common::Deadline& deadline) noexcept {
    return port_.write_all(data, deadline);
}

common::Result<std::size_t> Adapter::receive(const common::Span<std::uint8_t> buffer,
                                             const common::Deadline& deadline) noexcept {
    const common::Status status{wait_for_input(port_.descriptor(), cancel_descriptor_, deadline)};
    common::Result<std::size_t> result{common::Result<std::size_t>::failure(status)};
    if (is_ok(status)) {
        // The port is ready, so an expired deadline still takes what has arrived.
        const common::Result<std::size_t> received{port_.read(buffer, common::Deadline{})};
        const bool nothing{(!received.has_value()) && (received.status() == common::Status::kTimeout)};
        result = nothing ? common::Result<std::size_t>::success(0U) : received;
    }
    return result;
}

}  // namespace elm327
