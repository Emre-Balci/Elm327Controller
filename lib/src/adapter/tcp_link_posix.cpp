#include "detail/tcp_link.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/static_string.hpp"
#include "common/protocol/text.hpp"
#include "detail/poll_until.hpp"
#include "elm327/adapter/adapter.hpp"

namespace elm327 {
namespace detail {
namespace {

/// Waits for the connection: one, unless signal handlers interrupt it.
constexpr std::uint32_t kMaxConnectWaits{100U};

/// The address and the port of a device, NUL-terminated for getaddrinfo(): an IPv6 address with a
/// scope fits.
using HostText = common::StaticString<64U>;
using PortText = common::StaticString<5U>;

/// Splits the device @p path, "tcp:<address>:<port>" (an IPv6 address in brackets), into @p host and
/// @p port. @return false if it has another form, or the port is not a number from 1 to 65535.
bool split(const std::string_view path, HostText& host, PortText& port) noexcept {
    bool valid{path.size() > kTcpPrefix.size()};
    const std::string_view address{valid ? std::string_view{&path[kTcpPrefix.size()], path.size() - kTcpPrefix.size()}
                                         : std::string_view{}};
    const std::size_t colon{address.rfind(':')};
    valid = valid && (colon != std::string_view::npos) && (colon > 0U);
    if (valid) {
        std::string_view name{address.data(), colon};
        const std::string_view number{&address[colon + 1U], address.size() - colon - 1U};
        if ((name.size() >= 2U) && (name.front() == '[') && (name.back() == ']')) {
            name = std::string_view{&name[1U], name.size() - 2U};
        }
        const common::Result<std::uint16_t> value{
            common::parse_unsigned<std::uint16_t>(common::Span<const char>{number.data(), number.size()})};
        valid = (!name.empty()) && value.has_value() && (value.value() != 0U) && host.assign(name) &&
                port.assign(number);
    }
    return valid;
}

/// The Status of a failed connection, as the Adapter reports it.
common::Status connect_status(const int error) noexcept {
    common::Status status{common::Status::kSystemError};
    switch (error) {
        case ECONNREFUSED:  // Nothing listens at the port.
        case ECONNRESET:
        case EHOSTUNREACH:  // Not on the adapter's network.
        case ENETUNREACH:
        case ETIMEDOUT:  // The system gave up first.
            status = common::Status::kDisconnected;
            break;
        case EACCES:
        case EPERM:
            status = common::Status::kPermissionDenied;
            break;
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
            status = common::Status::kResourceLimit;
            break;
        default:
            break;
    }
    return status;
}

/// Makes @p descriptor non-blocking and closed on exec, and sends small writes at once (an adapter
/// reads one short command at a time). @return kOk, or kSystemError.
common::Status configure(const int descriptor) noexcept {
    const int flags{::fcntl(descriptor, F_GETFL)};
    const bool done{(flags >= 0) && (::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0) &&
                    (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0)};
    const int on{1};
    static_cast<void>(::setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)));  // Best effort.
    return done ? common::Status::kOk : common::Status::kSystemError;
}

/// Waits until the connection of @p descriptor is made or has failed, at most until @p deadline and
/// until @p cancel is readable. @return kOk once connected; otherwise why not.
common::Status wait_for_connection(const int descriptor, const common::NativeHandle cancel,
                                   const common::Deadline& deadline) noexcept {
    common::Status status{common::Status::kTimeout};
    bool waiting{true};
    for (std::uint32_t wait{0U}; waiting && (wait < kMaxConnectWaits); ++wait) {
        std::array<pollfd, 2U> entries{};
        entries[0U].fd = descriptor;
        entries[0U].events = POLLOUT;
        entries[1U].fd = cancel;  // poll() skips a negative descriptor.
        entries[1U].events = POLLIN;
        const int ready{poll_until(entries, deadline)};
        waiting = (ready < 0) && (errno == EINTR);
        if (waiting) {
            // A signal handler ran: wait again.
        } else if (ready < 0) {
            status = common::Status::kSystemError;
        } else if (ready == 0) {
            status = common::Status::kTimeout;
        } else if (static_cast<std::uint32_t>(entries[1U].revents) != 0U) {
            status = common::Status::kInterrupted;
        } else {
            int error{0};
            socklen_t size{sizeof(error)};
            status = (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &size) == 0) ? common::Status::kOk
                                                                                          : common::Status::kSystemError;
            if (is_ok(status) && (error != 0)) {
                status = connect_status(error);
            }
        }
    }
    return status;
}

}  // namespace

bool is_tcp_device(const char* const path) noexcept {
    const std::string_view text{(path != nullptr) ? std::string_view{path} : std::string_view{}};
    return (text.size() >= kTcpPrefix.size()) && (std::string_view{text.data(), kTcpPrefix.size()} == kTcpPrefix);
}

common::Result<common::NativeHandle> connect_tcp(const char* const path, const common::Deadline& deadline,
                                                 const common::NativeHandle cancel) noexcept {
    HostText host{};
    PortText port{};
    common::Status status{((path != nullptr) && split(std::string_view{path}, host, port))
                              ? common::Status::kOk
                              : common::Status::kInvalidArgument};
    // Numeric addresses only: getaddrinfo() then parses them without asking a name server.
    addrinfo* found{nullptr};
    if (is_ok(status)) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = static_cast<int>(static_cast<std::uint32_t>(AI_NUMERICHOST) |
                                          static_cast<std::uint32_t>(AI_NUMERICSERV));
        const bool parsed{(::getaddrinfo(host.c_str(), port.c_str(), &hints, &found) == 0) && (found != nullptr)};
        status = parsed ? common::Status::kOk : common::Status::kInvalidArgument;
    }
    int descriptor{-1};
    if (is_ok(status)) {
        descriptor = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        status = (descriptor >= 0) ? configure(descriptor) : connect_status(errno);
    }
    if (is_ok(status) && (::connect(descriptor, found->ai_addr, found->ai_addrlen) != 0)) {
        status = (errno == EINPROGRESS) ? wait_for_connection(descriptor, cancel, deadline) : connect_status(errno);
    }
    if (found != nullptr) {
        ::freeaddrinfo(found);
    }
    if ((!is_ok(status)) && (descriptor >= 0)) {
        static_cast<void>(::close(descriptor));
    }
    return is_ok(status) ? common::Result<common::NativeHandle>::success(descriptor)
                         : common::Result<common::NativeHandle>::failure(status);
}

}  // namespace detail
}  // namespace elm327
