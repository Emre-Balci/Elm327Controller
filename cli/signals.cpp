#include "cli/signals.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

#include "common/time/clock.hpp"

namespace elm327 {
namespace cli {
namespace {

/// Each round of the wait loop ends the wait or is a restart after EINTR.
constexpr std::uint32_t kMaxWaitRounds{1000U};

/// The signals that stop the program.
sigset_t stop_signal_set() noexcept {
    sigset_t set{};
    static_cast<void>(::sigemptyset(&set));
    static_cast<void>(::sigaddset(&set, SIGINT));
    static_cast<void>(::sigaddset(&set, SIGTERM));
    static_cast<void>(::sigaddset(&set, SIGHUP));
    static_cast<void>(::sigaddset(&set, SIGPIPE));
    return set;
}

timespec to_relative_timespec(const common::Duration duration) noexcept {
    const std::int64_t ns{duration.count()};
    timespec ts{};
    ts.tv_sec = ns / common::MonotonicClock::kNanosPerSecond;
    ts.tv_nsec = ns % common::MonotonicClock::kNanosPerSecond;
    return ts;
}

}  // namespace

StopSignals::~StopSignals() {
    if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
    }
}

common::Status StopSignals::open() noexcept {
    common::Status status{(descriptor_ < 0) ? common::Status::kOk : common::Status::kInvalidArgument};
    const sigset_t set{stop_signal_set()};
    if (is_ok(status)) {
        // A blocked signal stays pending instead of ending the process, and the signalfd reports it.
        // A write into a closed pipe then fails with EPIPE instead of killing the process.
        const bool blocked{::pthread_sigmask(SIG_BLOCK, &set, nullptr) == 0};
        status = blocked ? common::Status::kOk : common::Status::kSystemError;
    }
    if (is_ok(status)) {
        const int flags{
            static_cast<int>(static_cast<std::uint32_t>(SFD_CLOEXEC) | static_cast<std::uint32_t>(SFD_NONBLOCK))};
        descriptor_ = ::signalfd(-1, &set, flags);
        status = (descriptor_ >= 0) ? common::Status::kOk : common::Status::kSystemError;
    }
    return status;
}

std::int32_t StopSignals::received() noexcept {
    if ((signal_ == 0) && (descriptor_ >= 0)) {
        signalfd_siginfo info{};
        const ssize_t count{::read(descriptor_, &info, sizeof(info))};
        if (count == static_cast<ssize_t>(sizeof(info))) {
            signal_ = static_cast<std::int32_t>(info.ssi_signo);
        }
    }
    return signal_;
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

}  // namespace cli
}  // namespace elm327
