#include "elm327/adapter/adapter.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace elm327 {
namespace {

/// Waits per operation: a wait that ends before its deadline (the waits count in milliseconds and can
/// end up to a timer tick early) is repeated. The bound only guards against a misbehaving driver.
constexpr std::uint32_t kMaxWaits{1000U};

/// The longest single wait in milliseconds (INFINITE is 0xFFFFFFFF). Deadline::never() waits in such
/// steps, of about 49 days each.
constexpr DWORD kMaxWaitMilliseconds{0xFFFFFFFEU};

constexpr std::int64_t kNanosPerMillisecond{1'000'000};

/// Attempts to open a named pipe: one more after waiting for a busy one.
constexpr std::uint32_t kPipeAttempts{2U};

/// How long open() waits for a busy pipe: elm327sim takes the next connection a moment after the
/// last one closed.
constexpr DWORD kPipeWaitMilliseconds{2000U};

/// Reads of discard_input() on a pipe, and the bytes each takes.
constexpr std::uint32_t kMaxDiscards{64U};
constexpr std::size_t kDiscardSize{256U};

/// Room for the path of a pipe in UTF-16, with its NUL.
constexpr std::size_t kMaxWidePath{512U};

/// "pipe" in "\\.\pipe\<name>", in either case.
constexpr std::string_view kPipeLower{"pipe"};
constexpr std::string_view kPipeUpper{"PIPE"};

bool is_separator(const char character) noexcept {
    return (character == '\\') || (character == '/');
}

/// @return true for the path of a named pipe, "\\.\pipe\<name>" (with either separator).
bool is_pipe_path(const char* const path) noexcept {
    const std::string_view text{path};
    constexpr std::size_t kNameStart{4U};  // After "\\.\".
    bool pipe{(text.size() > (kNameStart + kPipeLower.size() + 1U)) && is_separator(text[0U]) &&
              is_separator(text[1U]) && (text[2U] == '.') && is_separator(text[3U]) &&
              is_separator(text[kNameStart + kPipeLower.size()])};
    for (std::size_t index{0U}; pipe && (index < kPipeLower.size()); ++index) {
        const char character{text[kNameStart + index]};
        pipe = (character == kPipeLower[index]) || (character == kPipeUpper[index]);
    }
    return pipe;
}

/// The Status of a failed read or write, as common::SerialPort reports it: a device or a pipe that
/// went away is kDisconnected.
common::Status transfer_status(const DWORD error) noexcept {
    common::Status status{common::Status::kSystemError};
    switch (error) {
        case ERROR_BROKEN_PIPE:         // The simulator closed its end of the pipe (a read).
        case ERROR_NO_DATA:             // Likewise, for a write.
        case ERROR_PIPE_NOT_CONNECTED:  // Likewise, after it disconnected.
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_DEVICE_REMOVED:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_NO_SUCH_DEVICE:
        case ERROR_BAD_COMMAND:  // USB serial drivers report an unplugged adapter with one of these,
        case ERROR_GEN_FAILURE:  // depending on the driver.
        case ERROR_ACCESS_DENIED:
        case ERROR_OPERATION_ABORTED:  // The driver aborted the operation; it was not cancelled here.
            status = common::Status::kDisconnected;
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
        case ERROR_NO_SYSTEM_RESOURCES:
            status = common::Status::kResourceLimit;
            break;
        default:
            break;
    }
    return status;
}

/// The Status of a failed open of a pipe, as common::SerialPort::open() reports it for a device.
common::Status open_status(const DWORD error) noexcept {
    common::Status status{common::Status::kSystemError};
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            status = common::Status::kNotFound;
            break;
        case ERROR_ACCESS_DENIED:
            status = common::Status::kPermissionDenied;
            break;
        case ERROR_PIPE_BUSY:  // Another program is connected, like a COM port in use.
        case ERROR_SEM_TIMEOUT:
        case ERROR_TOO_MANY_OPEN_FILES:
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_NO_SYSTEM_RESOURCES:
            status = common::Status::kResourceLimit;
            break;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_INVALID_PARAMETER:
            status = common::Status::kInvalidArgument;
            break;
        default:
            break;
    }
    return status;
}

/// The milliseconds until @p deadline, rounded up so that a wait does not end before it; 0 once it
/// has passed.
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

/// Waits until @p deadline for the overlapped operation that @p overlapped describes to complete, or
/// until @p cancel (nullptr for none) is signaled, cancels the operation if it has not completed by
/// then, and collects its result. Once this returns, the operation no longer uses @p overlapped or
/// its buffer.
/// @return the number of bytes transferred, also by an operation that was cancelled after it had
///         transferred some; kInterrupted (cancel), kTimeout, or the error of the operation.
common::Result<std::size_t> complete(const HANDLE handle, OVERLAPPED& overlapped, const HANDLE cancel,
                                     const common::Deadline& deadline) noexcept {
    const std::array<HANDLE, 2U> events{{overlapped.hEvent, cancel}};
    const DWORD count{(cancel != nullptr) ? 2U : 1U};
    bool completed{false};
    bool waiting{true};
    common::Status stop{common::Status::kTimeout};  // Why an operation that did not complete ended.
    for (std::uint32_t wait{0U}; waiting && (wait < kMaxWaits); ++wait) {
        // When both are signaled, the completion wins: bytes read are never dropped.
        const DWORD outcome{::WaitForMultipleObjects(count, events.data(), FALSE, wait_milliseconds(deadline))};
        if (outcome == WAIT_OBJECT_0) {
            completed = true;
            waiting = false;
        } else if (outcome == (WAIT_OBJECT_0 + 1U)) {
            stop = common::Status::kInterrupted;
            waiting = false;
        } else if (outcome != WAIT_TIMEOUT) {
            stop = common::Status::kSystemError;
            waiting = false;
        } else if (deadline.expired()) {
            waiting = false;
        } else {
            // Woke before the deadline: wait again.
        }
    }
    if (!completed) {
        // Fails with ERROR_NOT_FOUND when the operation completed in the meantime; either way it is
        // over once GetOverlappedResult() returns.
        static_cast<void>(::CancelIoEx(handle, &overlapped));
    }
    DWORD transferred{0U};
    common::Result<std::size_t> result{common::Result<std::size_t>::failure(common::Status::kSystemError)};
    // TRUE: waits until the operation has completed, which a cancelled one does promptly.
    if (::GetOverlappedResult(handle, &overlapped, &transferred, TRUE) != FALSE) {
        result = common::Result<std::size_t>::success(transferred);
    } else {
        const DWORD error{::GetLastError()};
        result = ((!completed) && (error == ERROR_OPERATION_ABORTED))
                     ? common::Result<std::size_t>::success(transferred)
                     : common::Result<std::size_t>::failure(transfer_status(error));
    }
    if ((!completed) && result.has_value() && (result.value() == 0U)) {
        result = common::Result<std::size_t>::failure(stop);
    }
    return result;
}

/// One overlapped read of up to buffer.size() bytes from @p handle, with the manual-reset @p event;
/// see complete() for @p cancel and @p deadline.
common::Result<std::size_t> read_overlapped(const HANDLE handle, const HANDLE event,
                                            const common::Span<std::uint8_t> buffer, const HANDLE cancel,
                                            const common::Deadline& deadline) noexcept {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    common::Result<std::size_t> result{common::Result<std::size_t>::failure(common::Status::kSystemError)};
    // ReadFile() resets the event when it starts. It returns TRUE when the read completed at once and
    // FALSE with ERROR_IO_PENDING when it goes on; complete() collects the result in both cases.
    if ((::ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped) != FALSE) ||
        (::GetLastError() == ERROR_IO_PENDING)) {
        result = complete(handle, overlapped, cancel, deadline);
    } else {
        result = common::Result<std::size_t>::failure(transfer_status(::GetLastError()));
    }
    return result;
}

/// Opens the named pipe @p path for overlapped I/O, as common::SerialPort opens a COM port. A busy
/// pipe is waited for briefly, since elm327sim lets the next program in a moment after the last one
/// closed it.
common::Status open_pipe(const char* const path, common::NativeHandle& pipe) noexcept {
    // The wide-character API takes the path in UTF-16; the "A" functions would read it in the code
    // page of the process.
    std::array<wchar_t, kMaxWidePath> name{};
    const int converted{::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, name.data(),
                                              static_cast<int>(name.size()))};
    common::Status status{(converted > 0) ? common::Status::kOk : common::Status::kInvalidArgument};
    HANDLE handle{INVALID_HANDLE_VALUE};
    for (std::uint32_t attempt{0U}; is_ok(status) && (handle == INVALID_HANDLE_VALUE) && (attempt < kPipeAttempts);
         ++attempt) {
        // No sharing, like a COM port. SECURITY_IDENTIFICATION: the program at the other end may
        // identify the user, but not act as the user.
        handle = ::CreateFileW(name.data(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error{::GetLastError()};
            if ((error != ERROR_PIPE_BUSY) || ((attempt + 1U) == kPipeAttempts)) {
                status = open_status(error);
            } else if (::WaitNamedPipeW(name.data(), kPipeWaitMilliseconds) == FALSE) {
                status = open_status(::GetLastError());  // Still busy (ERROR_SEM_TIMEOUT), or gone.
            } else {
                // Free again: the next attempt connects.
            }
        }
    }
    if (is_ok(status)) {
        pipe = handle;
    }
    return status;
}

}  // namespace

common::Status Adapter::open_device(const char* const path, const common::SerialOptions& options) noexcept {
    common::Status status{((path == nullptr) || is_open()) ? common::Status::kInvalidArgument : common::Status::kOk};
    if (is_ok(status)) {
        // A pipe has no line settings: the simulator behind it answers at any speed.
        status = is_pipe_path(path) ? open_pipe(path, pipe_) : port_.open(path, options);
    }
    if (is_ok(status)) {
        // Manual reset; ReadFile() and WriteFile() reset it when an operation starts.
        event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) {
            status = common::Status::kResourceLimit;
            static_cast<void>(close_device());
        }
    }
    return status;
}

common::Status Adapter::close_device() noexcept {
    common::Status status{port_.close()};  // kOk when it is not open.
    if (pipe_ != common::kInvalidNativeHandle) {
        // The handle is released even when CloseHandle() reports an error; never retry.
        status = (::CloseHandle(pipe_) != FALSE) ? common::Status::kOk : common::Status::kSystemError;
        pipe_ = common::kInvalidNativeHandle;
    }
    if (event_ != common::kInvalidNativeHandle) {
        static_cast<void>(::CloseHandle(event_));
        event_ = common::kInvalidNativeHandle;
    }
    return status;
}

common::Status Adapter::discard_input() noexcept {
    common::Status status{is_open() ? common::Status::kOk : common::Status::kInvalidArgument};
    if (is_ok(status) && (pipe_ == common::kInvalidNativeHandle)) {
        status = port_.discard_input();
    } else if (is_ok(status)) {
        // A pipe cannot be purged like a COM port: read what has arrived.
        std::array<std::uint8_t, kDiscardSize> scratch{};
        bool more{true};
        for (std::uint32_t round{0U}; is_ok(status) && more && (round < kMaxDiscards); ++round) {
            DWORD available{0U};
            if (::PeekNamedPipe(pipe_, nullptr, 0U, nullptr, &available, nullptr) == FALSE) {
                status = transfer_status(::GetLastError());
            } else if (available == 0U) {
                more = false;
            } else {
                // The bytes are there, so an expired deadline still takes them.
                const common::Result<std::size_t> count{
                    read_overlapped(pipe_, event_, scratch, nullptr, common::Deadline{})};
                status = count.has_value() ? common::Status::kOk : count.status();
            }
        }
    } else {
        // Not open.
    }
    return status;
}

common::Status Adapter::write_all(const common::Span<const std::uint8_t> data,
                                  const common::Deadline& deadline) noexcept {
    common::Status status{is_open() ? common::Status::kOk : common::Status::kInvalidArgument};
    if (is_ok(status) && (pipe_ == common::kInvalidNativeHandle)) {
        status = port_.write_all(data, deadline);
    } else if (is_ok(status)) {
        std::size_t written{0U};
        // Each round writes at least one byte or ends the loop.
        for (std::size_t round{0U}; is_ok(status) && (written < data.size()) && (round < data.size()); ++round) {
            const common::Span<const std::uint8_t> rest{data.subspan(written)};
            OVERLAPPED overlapped{};
            overlapped.hEvent = event_;
            common::Result<std::size_t> count{common::Result<std::size_t>::failure(common::Status::kSystemError)};
            if ((::WriteFile(pipe_, rest.data(), static_cast<DWORD>(rest.size()), nullptr, &overlapped) != FALSE) ||
                (::GetLastError() == ERROR_IO_PENDING)) {
                count = complete(pipe_, overlapped, nullptr, deadline);
            } else {
                count = common::Result<std::size_t>::failure(transfer_status(::GetLastError()));
            }
            if (!count.has_value()) {
                status = count.status();  // kTimeout once the deadline passes without progress.
            } else if (count.value() == 0U) {
                status = common::Status::kSystemError;  // A pipe takes every byte of a completed write.
            } else {
                written += count.value();
            }
        }
    } else {
        // Not open.
    }
    return status;
}

common::Result<std::size_t> Adapter::receive(const common::Span<std::uint8_t> buffer,
                                             const common::Deadline& deadline) noexcept {
    common::Result<std::size_t> result{common::Result<std::size_t>::failure(common::Status::kInterrupted)};
    // As on Linux, a stop request wins over input that has arrived, which stays queued.
    const bool cancelled{(cancel_descriptor_ != common::kInvalidNativeHandle) &&
                         (::WaitForSingleObject(cancel_descriptor_, 0U) == WAIT_OBJECT_0)};
    if (!is_open()) {
        result = common::Result<std::size_t>::failure(common::Status::kInvalidArgument);
    } else if (!cancelled) {
        // A read of a COM port completes as soon as a byte has arrived (common::SerialPort sets its
        // timeouts so), and so does a read of a pipe.
        const HANDLE handle{(pipe_ != common::kInvalidNativeHandle) ? pipe_ : port_.descriptor()};
        result = read_overlapped(handle, event_, buffer, cancel_descriptor_, deadline);
    } else {
        // kInterrupted.
    }
    return result;
}

}  // namespace elm327
