#include "elm327/controller/signals.hpp"

#include <signal.h>
#if COMMON_LINUX_API
#include <sys/signalfd.h>
#else
#include <fcntl.h>

#include <array>
#include <atomic>
#include <cerrno>
#endif
#include <unistd.h>

#include <cstddef>
#include <cstdint>

namespace elm327 {
namespace {

/// Reads per take_stop_request(): far more signals than can be pending, since the kernel merges
/// repeated standard signals.
constexpr std::uint32_t kMaxTakes{64U};

#if COMMON_LINUX_API
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
#else
// Without a signalfd (macOS, iOS), a handler writes the number of each signal into a pipe, whose read
// end is the descriptor. An Event's pipe carries zeros (see event_posix.cpp), which stand for SIGINT.

/// The signals that stop the program.
constexpr std::array<int, 4U> kStopSignals{{SIGINT, SIGTERM, SIGHUP, SIGPIPE}};

/// The write end of the pipe of the open StopSignals, for the handler; -1 while none is open.
std::atomic<int> g_write_end{-1};

/// What the signals did before StopSignals::open(), put back when it closes.
std::array<struct sigaction, kStopSignals.size()> g_previous{};

/// The handler: async-signal-safe, it only writes one byte.
void on_stop_signal(const int signal) noexcept {
    const int saved_errno{errno};
    const int descriptor{g_write_end.load(std::memory_order_relaxed)};
    if (descriptor >= 0) {
        const std::uint8_t number{static_cast<std::uint8_t>(signal)};
        const ssize_t written{::write(descriptor, &number, sizeof(number))};
        static_cast<void>(written);  // A full pipe has a signal to report already.
    }
    errno = saved_errno;
}

/// Makes an end of the pipe non-blocking and closed on exec. @return true if both worked.
bool configure(const int descriptor) noexcept {
    const int flags{::fcntl(descriptor, F_GETFL)};
    return (flags >= 0) && (::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0) &&
           (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0);
}

/// Puts back what the stop signals did before, from the first @p count of them.
void restore_handlers(const std::size_t count) noexcept {
    for (std::size_t index{0U}; index < count; ++index) {
        static_cast<void>(::sigaction(kStopSignals[index], &g_previous[index], nullptr));
    }
}
#endif

}  // namespace

#if COMMON_LINUX_API
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
#else
StopSignals::~StopSignals() {
    if (descriptor_ != common::kInvalidNativeHandle) {
        restore_handlers(kStopSignals.size());
        const int write_end{g_write_end.exchange(-1)};
        static_cast<void>(::close(write_end));
        static_cast<void>(::close(descriptor_));
    }
}

common::Status StopSignals::open() noexcept {
    // One per process, as the handlers are the process's.
    common::Status status{((descriptor_ == common::kInvalidNativeHandle) && (g_write_end.load() < 0))
                              ? common::Status::kOk
                              : common::Status::kInvalidArgument};
    std::array<int, 2U> ends{{-1, -1}};
    if (is_ok(status)) {
        const bool created{::pipe(ends.data()) == 0};
        status = (created && configure(ends[0U]) && configure(ends[1U])) ? common::Status::kOk
                                                                           : common::Status::kSystemError;
        if (created && (!is_ok(status))) {
            static_cast<void>(::close(ends[0U]));
            static_cast<void>(::close(ends[1U]));
        }
    }
    if (is_ok(status)) {
        g_write_end.store(ends[1U]);
        struct sigaction action {};
        action.sa_handler = &on_stop_signal;
        static_cast<void>(::sigemptyset(&action.sa_mask));
        action.sa_flags = SA_RESTART;  // Calls that a signal interrupts go on, except the waits.
        std::size_t installed{0U};
        for (; (installed < kStopSignals.size()) && is_ok(status); ++installed) {
            status = (::sigaction(kStopSignals[installed], &action, &g_previous[installed]) == 0)
                         ? common::Status::kOk
                         : common::Status::kSystemError;
        }
        if (is_ok(status)) {
            descriptor_ = ends[0U];
        } else {
            restore_handlers(installed);
            static_cast<void>(g_write_end.exchange(-1));
            static_cast<void>(::close(ends[0U]));
            static_cast<void>(::close(ends[1U]));
        }
    }
    return status;
}

std::int32_t StopSignals::received() noexcept {
    if ((signal_ == 0) && (descriptor_ != common::kInvalidNativeHandle)) {
        std::uint8_t number{0U};
        if (::read(descriptor_, &number, sizeof(number)) == static_cast<ssize_t>(sizeof(number))) {
            signal_ = static_cast<std::int32_t>(number);
        }
    }
    return signal_;
}

std::int32_t take_stop_request(const common::NativeHandle descriptor) noexcept {
    std::int32_t taken{0};
    bool more{descriptor >= 0};
    for (std::uint32_t round{0U}; more && (round < kMaxTakes); ++round) {
        // A byte per signal: its number from StopSignals, 0 from an Event, which counts as SIGINT.
        // The descriptor is non-blocking, so the loop ends when nothing is left.
        std::uint8_t number{0U};
        more = ::read(descriptor, &number, sizeof(number)) == static_cast<ssize_t>(sizeof(number));
        const std::int32_t signal{(number != 0U) ? static_cast<std::int32_t>(number) : SIGINT};
        if (more && ((taken == 0) || ends_program(signal))) {
            taken = signal;
        }
    }
    return taken;
}
#endif

bool ends_program(const std::int32_t signal) noexcept {
    return (signal == SIGTERM) || (signal == SIGHUP) || (signal == SIGPIPE);
}

}  // namespace elm327
