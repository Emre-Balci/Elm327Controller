#include "elm327/controller/bridged_link.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>

#include "detail/poll_until.hpp"

namespace elm327 {
namespace {

/// Waits for the answer: one, unless signal handlers interrupt it.
constexpr std::uint32_t kMaxWaits{100U};

/// The Status of the runtime's @p answer.
common::Status answer_status(const std::uint8_t answer) noexcept {
    common::Status status{common::Status::kDisconnected};  // LinkAnswer::kRefused, or an unknown answer.
    if (answer == static_cast<std::uint8_t>(LinkAnswer::kConnected)) {
        status = common::Status::kOk;
    } else if (answer == static_cast<std::uint8_t>(LinkAnswer::kNotFound)) {
        status = common::Status::kNotFound;
    } else if (answer == static_cast<std::uint8_t>(LinkAnswer::kNoPermission)) {
        status = common::Status::kPermissionDenied;
    } else if (answer == static_cast<std::uint8_t>(LinkAnswer::kOff)) {
        status = common::Status::kNotRunning;
    } else {
        // Refused.
    }
    return status;
}

/// Waits for the answer on @p end; see open_bridged_link().
common::Status wait_for_answer(const int end, const common::NativeHandle cancel,
                               const common::Deadline& deadline) noexcept {
    common::Status status{common::Status::kTimeout};
    bool waiting{true};
    for (std::uint32_t wait{0U}; waiting && (wait < kMaxWaits); ++wait) {
        std::array<pollfd, 2U> entries{};
        entries[0U].fd = end;
        entries[0U].events = POLLIN;
        entries[1U].fd = cancel;  // poll() skips a negative descriptor.
        entries[1U].events = POLLIN;
        const int ready{detail::poll_until(entries, deadline)};
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
            std::uint8_t answer{static_cast<std::uint8_t>(LinkAnswer::kRefused)};
            const bool answered{::read(end, &answer, sizeof(answer)) == static_cast<ssize_t>(sizeof(answer))};
            status = answered ? answer_status(answer) : common::Status::kDisconnected;  // Closed unanswered.
        }
    }
    return status;
}

}  // namespace

common::Result<common::NativeHandle> open_bridged_link(const char* const address, const LinkHandOver hand_over,
                                                       void* const context, const common::Deadline& deadline,
                                                       const common::NativeHandle cancel) noexcept {
    std::array<int, 2U> ends{{-1, -1}};
    common::Status status{((address != nullptr) && (hand_over != nullptr)) ? common::Status::kOk
                                                                           : common::Status::kInvalidArgument};
    if (is_ok(status)) {
        const bool created{::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0};
        status = created ? common::Status::kOk : common::Status::kResourceLimit;
    }
    if (is_ok(status)) {
        static_cast<void>(::fcntl(ends[0U], F_SETFD, FD_CLOEXEC));
        static_cast<void>(::fcntl(ends[1U], F_SETFD, FD_CLOEXEC));
        hand_over(address, ends[1U], context);  // The runtime owns that end from here on.
        status = wait_for_answer(ends[0U], cancel, deadline);
        if (!is_ok(status)) {
            static_cast<void>(::close(ends[0U]));  // The runtime finds the link closed and lets go.
        }
    }
    return is_ok(status) ? common::Result<common::NativeHandle>::success(ends[0U])
                         : common::Result<common::NativeHandle>::failure(status);
}

}  // namespace elm327
