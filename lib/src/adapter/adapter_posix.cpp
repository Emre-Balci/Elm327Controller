#include "elm327/adapter/adapter.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include "detail/poll_until.hpp"

namespace elm327 {
namespace {

/// Reads of discard_input() on a stream, and the bytes each takes.
constexpr std::uint32_t kMaxDiscards{64U};
constexpr std::size_t kDiscardSize{256U};

/// System calls per write_all() on a stream: a partial write, an EINTR restart or a wait for room
/// each take one. The bound only guards against a signal storm.
constexpr std::uint32_t kMaxWriteCalls{1000U};

/// The flags of send() on a stream: a peer that went away is EPIPE rather than a SIGPIPE that ends
/// the process. macOS and iOS lack MSG_NOSIGNAL and set SO_NOSIGPIPE on the socket instead.
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags{MSG_NOSIGNAL};
#else
constexpr int kSendFlags{0};
#endif

/// Waits until @p port has input or reports an error, @p cancel is readable, or @p deadline passes.
/// @return kOk, kInterrupted or kTimeout.
common::Status wait_for_input(const common::NativeHandle port, const common::NativeHandle cancel,
                              const common::Deadline& deadline) noexcept {
    std::array<pollfd, 2U> entries{};
    entries[0U].fd = port;
    entries[0U].events = POLLIN;
    entries[1U].fd = cancel;  // poll() skips a negative descriptor.
    entries[1U].events = POLLIN;
    const int ready{detail::poll_until(entries, deadline)};
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

/// Waits until @p stream takes more output or @p deadline passes. @return kOk (also for a hang-up or
/// an error, which the write then reports), kTimeout, or kSystemError.
common::Status wait_for_room(const common::NativeHandle stream, const common::Deadline& deadline) noexcept {
    std::array<pollfd, 1U> entries{};
    entries[0U].fd = stream;
    entries[0U].events = POLLOUT;
    const int ready{detail::poll_until(entries, deadline)};
    common::Status status{common::Status::kOk};
    if (ready < 0) {
        status = (errno == EINTR) ? common::Status::kOk : common::Status::kSystemError;
    } else if (ready == 0) {
        status = common::Status::kTimeout;
    } else {
        status = common::Status::kOk;
    }
    return status;
}

/// The Status of a failed read or write on a stream, as common::SerialPort reports it for a device:
/// a peer that went away is kDisconnected.
common::Status stream_status(const int error) noexcept {
    common::Status status{common::Status::kSystemError};
    switch (error) {
        case EPIPE:
        case ECONNRESET:
        case ECONNABORTED:
        case ENOTCONN:
        case ETIMEDOUT:  // The link timed out, e.g. a Bluetooth adapter out of reach.
        case EHOSTUNREACH:
        case ENETDOWN:
        case EIO:
            status = common::Status::kDisconnected;
            break;
        case ENOBUFS:
        case ENOMEM:
            status = common::Status::kResourceLimit;
            break;
        default:
            break;
    }
    return status;
}

}  // namespace

common::Status Adapter::open_device(const char* const path, const common::SerialOptions& options) noexcept {
    return is_open() ? common::Status::kInvalidArgument : port_.open(path, options);
}

common::Status Adapter::adopt_stream(const common::NativeHandle stream) noexcept {
    common::Status status{((stream != common::kInvalidNativeHandle) && (!is_open()))
                              ? common::Status::kOk
                              : common::Status::kInvalidArgument};
    if (is_ok(status)) {
        // Non-blocking, like the serial port: a read without data fails with EAGAIN.
        const int flags{::fcntl(stream, F_GETFL)};
        status = ((flags >= 0) && (::fcntl(stream, F_SETFL, flags | O_NONBLOCK) == 0)) ? common::Status::kOk
                                                                                       : common::Status::kSystemError;
    }
#if defined(SO_NOSIGPIPE)
    if (is_ok(status)) {
        const int on{1};
        status = (::setsockopt(stream, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0) ? common::Status::kOk
                                                                                       : common::Status::kSystemError;
    }
#endif
    if (is_ok(status)) {
        stream_ = stream;
    } else if (stream != common::kInvalidNativeHandle) {
        static_cast<void>(::close(stream));
    } else {
        // Nothing to close.
    }
    return status;
}

common::Status Adapter::close_device() noexcept {
    common::Status status{common::Status::kOk};
    if (stream_ != common::kInvalidNativeHandle) {
        // The descriptor is released even when close() reports an error.
        status = (::close(stream_) == 0) ? common::Status::kOk : common::Status::kSystemError;
        stream_ = common::kInvalidNativeHandle;
    } else {
        status = port_.close();
    }
    return status;
}

common::Status Adapter::discard_input() noexcept {
    common::Status status{common::Status::kOk};
    if (stream_ == common::kInvalidNativeHandle) {
        status = port_.discard_input();
    } else {
        // A socket has no input queue to flush: read what has arrived. A peer that went away shows in
        // the next read.
        std::array<std::uint8_t, kDiscardSize> scratch{};
        bool more{true};
        for (std::uint32_t round{0U}; more && (round < kMaxDiscards); ++round) {
            const ssize_t count{::read(stream_, scratch.data(), scratch.size())};
            more = (count > 0) || ((count < 0) && (errno == EINTR));
        }
    }
    return status;
}

common::Status Adapter::write_all(const common::Span<const std::uint8_t> data,
                                  const common::Deadline& deadline) noexcept {
    common::Status status{common::Status::kOk};
    if (stream_ == common::kInvalidNativeHandle) {
        status = port_.write_all(data, deadline);
    } else {
        common::Span<const std::uint8_t> rest{data};
        for (std::uint32_t call{0U}; is_ok(status) && (!rest.empty()) && (call < kMaxWriteCalls); ++call) {
            const ssize_t count{::send(stream_, rest.data(), rest.size(), kSendFlags)};
            if (count >= 0) {
                rest = rest.subspan(static_cast<std::size_t>(count));
            } else if (errno == EAGAIN) {  // EWOULDBLOCK is the same.
                status = wait_for_room(stream_, deadline);
            } else if (errno != EINTR) {
                status = stream_status(errno);
            } else {
                // Interrupted by a signal handler before anything was written: try again.
            }
        }
        if (is_ok(status) && (!rest.empty())) {
            status = common::Status::kTimeout;
        }
    }
    return status;
}

common::Result<std::size_t> Adapter::receive(const common::Span<std::uint8_t> buffer,
                                             const common::Deadline& deadline) noexcept {
    const common::NativeHandle device{(stream_ != common::kInvalidNativeHandle) ? stream_ : port_.descriptor()};
    const common::Status status{wait_for_input(device, cancel_descriptor_, deadline)};
    common::Result<std::size_t> result{common::Result<std::size_t>::failure(status)};
    if (is_ok(status) && (stream_ != common::kInvalidNativeHandle)) {
        const ssize_t count{::read(stream_, buffer.data(), buffer.size())};
        if (count > 0) {
            result = common::Result<std::size_t>::success(static_cast<std::size_t>(count));
        } else if (count == 0) {
            result = common::Result<std::size_t>::failure(common::Status::kDisconnected);  // The peer closed its end.
        } else if ((errno == EAGAIN) || (errno == EINTR)) {  // EWOULDBLOCK is EAGAIN.
            result = common::Result<std::size_t>::success(0U);
        } else {
            result = common::Result<std::size_t>::failure(stream_status(errno));
        }
    } else if (is_ok(status)) {
        // The port is ready, so an expired deadline still takes what has arrived.
        const common::Result<std::size_t> received{port_.read(buffer, common::Deadline{})};
        const bool nothing{(!received.has_value()) && (received.status() == common::Status::kTimeout)};
        result = nothing ? common::Result<std::size_t>::success(0U) : received;
    } else {
        // Interrupted, timed out, or the wait failed.
    }
    return result;
}

}  // namespace elm327
