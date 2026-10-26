#include "elm327/controller/timing.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>

#include <array>
#include <cerrno>

#include "detail/poll_until.hpp"
#endif

#include <cstdint>

namespace elm327 {
namespace {

/// Each round of the wait loop ends the wait or is a restart after EINTR (on Windows, after a wait
/// that ended before the deadline).
constexpr std::uint32_t kMaxWaitRounds{1000U};

#if defined(_WIN32)
/// The longest single wait in milliseconds (INFINITE is 0xFFFFFFFF); Deadline::never() waits in such
/// steps.
constexpr DWORD kMaxWaitMilliseconds{0xFFFFFFFEU};

constexpr std::int64_t kNanosPerMillisecond{1'000'000};

/// The milliseconds until @p deadline, rounded up so that the wait does not end before it.
DWORD wait_milliseconds(const common::Deadline& deadline) noexcept {
    DWORD milliseconds{kMaxWaitMilliseconds};
    if (!deadline.is_never()) {
        const std::int64_t nanoseconds{deadline.remaining().count()};
        const std::int64_t rounded_up{(nanoseconds / kNanosPerMillisecond) +
                                      (((nanoseconds % kNanosPerMillisecond) != 0) ? 1 : 0)};
        milliseconds = (rounded_up < std::int64_t{kMaxWaitMilliseconds}) ? static_cast<DWORD>(rounded_up)
                                                                         : kMaxWaitMilliseconds;
    }
    return milliseconds;
}
#endif

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

#if defined(_WIN32)
common::Status wait_until(const common::NativeHandle descriptor, const common::Deadline& deadline) noexcept {
    common::Status status{common::Status::kSystemError};
    bool done{false};
    for (std::uint32_t round{0U}; (!done) && (round < kMaxWaitRounds); ++round) {
        const DWORD milliseconds{wait_milliseconds(deadline)};
        DWORD outcome{WAIT_TIMEOUT};
        if (descriptor != common::kInvalidNativeHandle) {
            outcome = ::WaitForSingleObject(descriptor, milliseconds);
        } else {
            ::Sleep(milliseconds);
        }
        done = true;
        if (outcome == WAIT_OBJECT_0) {
            status = common::Status::kInterrupted;
        } else if (outcome != WAIT_TIMEOUT) {
            status = common::Status::kSystemError;
        } else if (deadline.expired()) {
            status = common::Status::kOk;
        } else {
            done = false;  // Woke before the deadline: wait for the rest of the time.
        }
    }
    return status;
}
#else
common::Status wait_until(const common::NativeHandle descriptor, const common::Deadline& deadline) noexcept {
    common::Status status{common::Status::kSystemError};
    bool done{false};
    for (std::uint32_t round{0U}; (!done) && (round < kMaxWaitRounds); ++round) {
        std::array<pollfd, 1U> entries{};
        entries[0U].fd = descriptor;  // poll() skips a negative descriptor.
        entries[0U].events = POLLIN;
        const int ready{detail::poll_until(entries, deadline)};
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
#endif

}  // namespace elm327
