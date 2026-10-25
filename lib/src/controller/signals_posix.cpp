#include "elm327/controller/signals.hpp"

#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>

namespace elm327 {
namespace {

/// Reads per take_stop_request(): far more signals than can be pending, since the kernel merges
/// repeated standard signals.
constexpr std::uint32_t kMaxTakes{64U};

/// Bytes of an eventfd counter.
constexpr std::size_t kEventSize{8U};

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

}  // namespace

StopSignals::~StopSignals() {
    if (descriptor_ != common::kInvalidNativeHandle) {
        static_cast<void>(::close(descriptor_));
    }
}

common::Status StopSignals::open() noexcept {
    common::Status status{(descriptor_ == common::kInvalidNativeHandle) ? common::Status::kOk
                                                                        : common::Status::kInvalidArgument};
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
    if ((signal_ == 0) && (descriptor_ != common::kInvalidNativeHandle)) {
        signalfd_siginfo info{};
        const ssize_t count{::read(descriptor_, &info, sizeof(info))};
        if (count == static_cast<ssize_t>(sizeof(info))) {
            signal_ = static_cast<std::int32_t>(info.ssi_signo);
        }
    }
    return signal_;
}

std::int32_t take_stop_request(const common::NativeHandle descriptor) noexcept {
    std::int32_t taken{0};
    bool more{descriptor >= 0};
    for (std::uint32_t round{0U}; more && (round < kMaxTakes); ++round) {
        // A signalfd hands out one signalfd_siginfo per read, an eventfd its 8-byte counter. Both
        // descriptors are non-blocking, so the loop ends when nothing is left.
        signalfd_siginfo info{};
        const ssize_t count{::read(descriptor, &info, sizeof(info))};
        std::int32_t signal{0};
        if (count == static_cast<ssize_t>(sizeof(info))) {
            signal = static_cast<std::int32_t>(info.ssi_signo);
        } else if (count == static_cast<ssize_t>(kEventSize)) {
            signal = SIGINT;
        } else {
            more = false;
        }
        if ((signal != 0) && ((taken == 0) || ends_program(signal))) {
            taken = signal;
        }
    }
    return taken;
}

bool ends_program(const std::int32_t signal) noexcept {
    return (signal == SIGTERM) || (signal == SIGHUP) || (signal == SIGPIPE);
}

}  // namespace elm327
