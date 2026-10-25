#include "elm327/controller/timing.hpp"

#include <poll.h>
#include <time.h>

#include <cerrno>
#include <cstdint>

namespace elm327 {
namespace {

/// Each round of the wait loop ends the wait or is a restart after EINTR.
constexpr std::uint32_t kMaxWaitRounds{1000U};

timespec to_relative_timespec(const common::Duration duration) noexcept {
    const std::int64_t ns{duration.count()};
    timespec ts{};
    ts.tv_sec = ns / common::MonotonicClock::kNanosPerSecond;
    ts.tv_nsec = ns % common::MonotonicClock::kNanosPerSecond;
    return ts;
}

}  // namespace

common::TimePoint next_activation(const common::TimePoint previous, const common::Duration interval,
                                  const common::TimePoint now) noexcept {
    common::TimePoint next{previous + interval};
    if (next <= now) {
        const std::int64_t missed{(now - next) / interval};
        next += interval * (missed + 1);
    }
    return next;
}

common::Status wait_until(const std::int32_t descriptor, const common::Deadline& deadline) noexcept {
    common::Status status{common::Status::kSystemError};
    bool done{false};
    for (std::uint32_t round{0U}; (!done) && (round < kMaxWaitRounds); ++round) {
        pollfd entry{};
        entry.fd = descriptor;  // poll() skips a negative descriptor.
        entry.events = POLLIN;
        timespec timeout{};
        const timespec* timeout_pointer{nullptr};
        if (!deadline.is_never()) {
            timeout = to_relative_timespec(deadline.remaining());
            timeout_pointer = &timeout;
        }
        const int ready{::ppoll(&entry, 1U, timeout_pointer, nullptr)};
        done = true;
        if (ready > 0) {
            status = common::Status::kInterrupted;
        } else if ((ready == 0) || deadline.expired()) {
            status = common::Status::kOk;
        } else if (errno == EINTR) {
            done = false;  // A signal handler ran: wait for the rest of the time.
        } else {
            status = common::Status::kSystemError;
        }
    }
    return status;
}

}  // namespace elm327
