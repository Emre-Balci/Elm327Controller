#include "elm327/controller/signals.hpp"

#include <signal.h>
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace elm327 {
namespace {

/// The signals a StopSignals reports, in ascending order, as on Linux.
constexpr std::array<std::int32_t, 3U> kStopSignals{{SIGINT, SIGTERM, SIGBREAK}};

/// How long the handler holds back the end of the process after the console window was closed:
/// Windows ends it when the handler returns, and at the latest after about five seconds, and main()
/// needs the time to close the serial port and return.
constexpr DWORD kCloseGraceMilliseconds{4000U};

/// What the handler shares with the threads that take the requests. The handler runs on a thread
/// that Windows starts for each event, so the lock keeps the event signaled exactly while a request
/// is pending: a wait that the event ends always finds one.
struct Shared final {
    SRWLOCK lock{SRWLOCK_INIT};
    HANDLE event{nullptr};        ///< The descriptor of the open StopSignals; nullptr if none is open.
    std::uint32_t pending{0U};    ///< One bit per entry of kStopSignals.
};

Shared& shared() noexcept {
    static Shared state{};
    return state;
}

/// The bit of @p signal in Shared::pending.
std::uint32_t bit_of(const std::int32_t signal) noexcept {
    std::uint32_t bit{0U};
    for (std::size_t index{0U}; index < kStopSignals.size(); ++index) {
        if (kStopSignals[index] == signal) {
            bit = std::uint32_t{1U} << index;
        }
    }
    return bit;
}

/// The console control handler. Runs on a thread of its own, created by Windows for the event.
BOOL WINAPI on_console_event(const DWORD type) noexcept {
    std::int32_t signal{0};
    bool closing{false};
    switch (type) {
        case CTRL_C_EVENT:
            signal = SIGINT;
            break;
        case CTRL_BREAK_EVENT:
            signal = SIGBREAK;
            break;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            signal = SIGTERM;
            closing = true;
            break;
        default:
            break;
    }
    bool handled{false};
    Shared& state{shared()};
    ::AcquireSRWLockExclusive(&state.lock);
    if ((signal != 0) && (state.event != nullptr)) {
        state.pending |= bit_of(signal);
        handled = ::SetEvent(state.event) != FALSE;
    }
    ::ReleaseSRWLockExclusive(&state.lock);
    if (handled && closing) {
        // The process ends when this returns: give main() the time to close the port first. Its
        // return ends the process, and this thread with it.
        ::Sleep(kCloseGraceMilliseconds);
    }
    return handled ? TRUE : FALSE;
}

/// Takes the signals pending on the event of the open StopSignals and resets it. The caller holds
/// the lock. @return the bits of the signals.
std::uint32_t take_pending(Shared& state) noexcept {
    const std::uint32_t pending{state.pending};
    state.pending = 0U;
    static_cast<void>(::ResetEvent(state.event));
    return pending;
}

}  // namespace

StopSignals::~StopSignals() {
    if (descriptor_ != common::kInvalidNativeHandle) {
        static_cast<void>(::SetConsoleCtrlHandler(&on_console_event, FALSE));
        Shared& state{shared()};
        ::AcquireSRWLockExclusive(&state.lock);
        state.event = nullptr;
        state.pending = 0U;
        ::ReleaseSRWLockExclusive(&state.lock);
        static_cast<void>(::CloseHandle(descriptor_));
    }
}

common::Status StopSignals::open() noexcept {
    common::Status status{(descriptor_ == common::kInvalidNativeHandle) ? common::Status::kOk
                                                                        : common::Status::kInvalidArgument};
    // Manual reset: it stays signaled until the request is taken.
    const HANDLE event{is_ok(status) ? ::CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr};
    if (is_ok(status) && (event == nullptr)) {
        status = common::Status::kSystemError;
    }
    Shared& state{shared()};
    if (is_ok(status)) {
        ::AcquireSRWLockExclusive(&state.lock);
        if (state.event == nullptr) {
            state.event = event;
            state.pending = 0U;
        } else {
            status = common::Status::kInvalidArgument;  // Another StopSignals has the handler.
        }
        ::ReleaseSRWLockExclusive(&state.lock);
    }
    if (is_ok(status) && (::SetConsoleCtrlHandler(&on_console_event, TRUE) == FALSE)) {
        status = common::Status::kSystemError;
        ::AcquireSRWLockExclusive(&state.lock);
        state.event = nullptr;
        ::ReleaseSRWLockExclusive(&state.lock);
    }
    if (is_ok(status)) {
        descriptor_ = event;
    } else if (event != nullptr) {
        static_cast<void>(::CloseHandle(event));
    } else {
        // Nothing was created.
    }
    return status;
}

std::int32_t StopSignals::received() noexcept {
    if ((signal_ == 0) && (descriptor_ != common::kInvalidNativeHandle)) {
        // Like a read of a signalfd: the lowest pending signal, the others stay pending.
        Shared& state{shared()};
        ::AcquireSRWLockExclusive(&state.lock);
        for (std::size_t index{0U}; (signal_ == 0) && (index < kStopSignals.size()); ++index) {
            const std::uint32_t bit{std::uint32_t{1U} << index};
            if ((state.pending & bit) != 0U) {
                signal_ = kStopSignals[index];
                state.pending &= ~bit;
            }
        }
        if (state.pending == 0U) {
            static_cast<void>(::ResetEvent(state.event));
        }
        ::ReleaseSRWLockExclusive(&state.lock);
    }
    return signal_;
}

std::int32_t take_stop_request(const common::NativeHandle descriptor) noexcept {
    std::int32_t taken{0};
    if (descriptor != common::kInvalidNativeHandle) {
        Shared& state{shared()};
        ::AcquireSRWLockExclusive(&state.lock);
        if (descriptor == state.event) {
            const std::uint32_t pending{take_pending(state)};
            for (std::size_t index{0U}; index < kStopSignals.size(); ++index) {
                const std::int32_t signal{kStopSignals[index]};
                const bool arrived{(pending & (std::uint32_t{1U} << index)) != 0U};
                if (arrived && ((taken == 0) || ends_program(signal))) {
                    taken = signal;
                }
            }
        } else if (::WaitForSingleObject(descriptor, 0U) == WAIT_OBJECT_0) {
            // An event of the caller's: signaled once or more, like an eventfd counter.
            static_cast<void>(::ResetEvent(descriptor));
            taken = SIGINT;
        } else {
            // Nothing pending.
        }
        ::ReleaseSRWLockExclusive(&state.lock);
    }
    return taken;
}

bool ends_program(const std::int32_t signal) noexcept {
    return (signal == SIGTERM) || (signal == SIGBREAK);
}

}  // namespace elm327
