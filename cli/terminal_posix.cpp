#include "cli/terminal.hpp"

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace elm327 {
namespace cli {
namespace {

/// Restarts of a system call after EINTR.
constexpr std::uint32_t kMaxRestarts{1000U};

}  // namespace

LineInput::LineInput(const common::NativeHandle input, Console& out,
                     const common::NativeHandle cancel_descriptor) noexcept
    : input_{input},
      out_{&out},
      cancel_descriptor_{cancel_descriptor},
      editing_{(::isatty(input) == 1) && (::isatty(out.descriptor()) == 1)} {}

common::Status LineInput::wait() const noexcept {
    common::Status status{common::Status::kSystemError};
    bool done{false};
    for (std::uint32_t round{0U}; (!done) && (round < kMaxRestarts); ++round) {
        std::array<pollfd, 2U> entries{};
        entries[0U].fd = input_;
        entries[0U].events = POLLIN;
        entries[1U].fd = cancel_descriptor_;  // poll() skips a negative descriptor.
        entries[1U].events = POLLIN;
        // A person takes as long as they like: no timeout.
        const int ready{::poll(entries.data(), entries.size(), -1)};
        if (ready > 0) {
            status = (static_cast<std::uint32_t>(entries[1U].revents) != 0U) ? common::Status::kInterrupted
                                                                             : common::Status::kOk;
            done = true;
        } else if ((ready < 0) && (errno == EINTR)) {
            // A signal handler ran: wait again.
        } else {
            done = true;
        }
    }
    return status;
}

common::Result<std::size_t> LineInput::read_bytes(const common::Span<char> buffer) const noexcept {
    using SizeResult = common::Result<std::size_t>;
    SizeResult result{SizeResult::failure(common::Status::kSystemError)};
    bool done{false};
    for (std::uint32_t round{0U}; (!done) && (round < kMaxRestarts); ++round) {
        const ssize_t count{::read(input_, buffer.data(), buffer.size())};
        if (count >= 0) {
            result = SizeResult::success(static_cast<std::size_t>(count));
            done = true;
        } else if (errno == EINTR) {
            // Interrupted before anything was read: read again.
        } else if (errno == EIO) {
            result = SizeResult::success(0U);  // The terminal hung up: the end of the input.
            done = true;
        } else {
            done = true;
        }
    }
    return result;
}

common::Status LineInput::enter_raw_mode() noexcept {
    common::Status status{common::Status::kOk};
    if (!raw_) {
        status = (::tcgetattr(input_, &saved_) == 0) ? common::Status::kOk : common::Status::kSystemError;
        if (is_ok(status)) {
            // Every key arrives at once, without echo and without the signals of Ctrl+C and Ctrl+Z.
            // Output processing stays on, so "\n" still starts a new line.
            termios raw{saved_};
            raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
            raw.c_cflag |= static_cast<tcflag_t>(CS8);
            raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
            raw.c_cc[VMIN] = static_cast<cc_t>(1U);
            raw.c_cc[VTIME] = static_cast<cc_t>(0U);
            status = (::tcsetattr(input_, TCSANOW, &raw) == 0) ? common::Status::kOk : common::Status::kSystemError;
        }
        raw_ = is_ok(status);
    }
    return status;
}

void LineInput::leave_raw_mode() noexcept {
    if (raw_) {
        // Nothing better to do if the terminal refuses: it is gone, or it was never changed.
        static_cast<void>(::tcsetattr(input_, TCSANOW, &saved_));
        raw_ = false;
    }
}

std::size_t LineInput::columns() const noexcept {
    winsize size{};
    const bool known{(::ioctl(out_->descriptor(), TIOCGWINSZ, &size) == 0) && (size.ws_col > 0U)};
    return known ? std::size_t{size.ws_col} : kDefaultColumns;
}

}  // namespace cli
}  // namespace elm327
