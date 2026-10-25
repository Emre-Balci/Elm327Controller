#include "cli/terminal.hpp"

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "common/containers/static_string.hpp"
#include "common/protocol/text.hpp"
#include "elm327/controller/signals.hpp"

namespace elm327 {
namespace cli {
namespace {

/// Rounds of the read loops: each one handles at least one byte or ends the line, so this bounds
/// the length of what a person can type, not the time they take.
constexpr std::uint32_t kMaxRounds{std::numeric_limits<std::uint32_t>::max()};

/// Restarts of a system call after EINTR.
constexpr std::uint32_t kMaxRestarts{1000U};

/// The width of a terminal that does not tell it.
constexpr std::size_t kDefaultColumns{80U};

constexpr std::string_view kClearToEnd{"\x1b[0K"};
constexpr std::string_view kClearScreen{"\x1b[H\x1b[2J"};
constexpr std::string_view kCursorRight{"\x1b["};  ///< Followed by the number of columns and 'C'.
constexpr std::string_view kBell{"\a"};

/// A drawn line: the prompt, the line and a few escape sequences.
using Output = common::StaticString<LineEditor::kMaxLineLength + 128U>;

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

std::size_t columns(const std::int32_t descriptor) noexcept {
    winsize size{};
    const bool known{(::ioctl(descriptor, TIOCGWINSZ, &size) == 0) && (size.ws_col > 0U)};
    return known ? std::size_t{size.ws_col} : kDefaultColumns;
}

}  // namespace

LineInput::LineInput(const std::int32_t input, Console& out, const std::int32_t cancel_descriptor) noexcept
    : input_{input},
      out_{&out},
      cancel_descriptor_{cancel_descriptor},
      editing_{(::isatty(input) == 1) && (::isatty(out.descriptor()) == 1)} {}

LineInput::~LineInput() {
    leave_raw_mode();
}

InputEvent LineInput::read(const std::string_view prompt) noexcept {
    line_ = common::Span<const char>{};
    signal_ = 0;
    InputEvent event{InputEvent::kFailed};
    if (editing_ && (!is_ok(enter_raw_mode()))) {
        editing_ = false;  // A terminal without raw mode still gives plain lines.
    }
    if (editing_) {
        event = read_edited(prompt);
        leave_raw_mode();
    } else {
        event = read_plain();
    }
    return event;
}

InputEvent LineInput::read_edited(const std::string_view prompt) noexcept {
    InputEvent event{InputEvent::kFailed};
    editor_.start();
    draw(prompt);
    bool done{false};
    for (std::uint32_t round{0U}; (!done) && (round < kMaxRounds); ++round) {
        if (pending_begin_ == pending_end_) {
            const common::Status ready{wait()};
            if (ready == common::Status::kInterrupted) {
                event = take_stop();
                done = true;
            } else if (!is_ok(ready)) {
                done = true;
            } else {
                const common::Result<std::size_t> count{read_bytes(pending_)};
                if (!count.has_value()) {
                    done = true;
                } else if (count.value() == 0U) {
                    event = InputEvent::kEnd;
                    done = true;
                } else {
                    pending_begin_ = 0U;
                    pending_end_ = count.value();
                }
            }
        }
        // Bytes after the end of the line stay pending for the next line, e.g. of a paste.
        while ((!done) && (pending_begin_ < pending_end_)) {
            const char byte{pending_[pending_begin_]};
            ++pending_begin_;
            switch (editor_.feed(byte)) {
                case LineEditor::Action::kNone:
                    break;
                case LineEditor::Action::kRedraw:
                    draw(prompt);
                    break;
                case LineEditor::Action::kAccept:
                    line_ = editor_.line();
                    event = InputEvent::kLine;
                    done = true;
                    break;
                case LineEditor::Action::kCancel:
                    event = InputEvent::kCancelled;
                    done = true;
                    break;
                case LineEditor::Action::kEnd:
                    event = InputEvent::kEnd;
                    done = true;
                    break;
                case LineEditor::Action::kClearScreen:
                    out_->write(span_of(kClearScreen));
                    draw(prompt);
                    break;
                case LineEditor::Action::kList:
                    list_matches();
                    draw(prompt);
                    break;
                case LineEditor::Action::kBell:
                    out_->write(span_of(kBell));
                    break;
            }
        }
    }
    // The output of the command, or the next prompt, starts on a line of its own.
    out_->write(span_of((event == InputEvent::kCancelled) ? "^C\n" : "\n"));
    return event;
}

InputEvent LineInput::read_plain() noexcept {
    InputEvent event{InputEvent::kFailed};
    bool done{false};
    for (std::uint32_t round{0U}; (!done) && (round < kMaxRounds); ++round) {
        std::size_t newline{plain_end_};
        for (std::size_t index{plain_begin_}; index < plain_end_; ++index) {
            if (plain_[index] == '\n') {
                newline = index;
                break;
            }
        }
        if (newline < plain_end_) {
            std::size_t end{newline};
            if ((end > plain_begin_) && (plain_[end - 1U] == '\r')) {
                --end;  // A "\r\n" line end.
            }
            line_ = common::Span<const char>{plain_}.subspan(plain_begin_, end - plain_begin_);
            plain_begin_ = newline + 1U;
            event = skipping_ ? InputEvent::kTooLong : InputEvent::kLine;
            skipping_ = false;
            done = true;
        } else if (end_of_input_) {
            // The last line, without a line end.
            line_ = common::Span<const char>{plain_}.subspan(plain_begin_, plain_end_ - plain_begin_);
            event = skipping_ ? InputEvent::kTooLong : (line_.empty() ? InputEvent::kEnd : InputEvent::kLine);
            plain_begin_ = plain_end_;
            skipping_ = false;
            done = true;
        } else {
            // Make room: move the start of the line to the front of the buffer.
            const std::size_t kept{plain_end_ - plain_begin_};
            for (std::size_t index{0U}; index < kept; ++index) {
                plain_[index] = plain_[plain_begin_ + index];
            }
            plain_begin_ = 0U;
            plain_end_ = kept;
            if (plain_end_ == plain_.size()) {
                skipping_ = true;  // Longer than kMaxPlainLine: dropped up to its end.
                plain_end_ = 0U;
            }
            const common::Status ready{wait()};
            if (ready == common::Status::kInterrupted) {
                event = take_stop();
                done = true;
            } else if (!is_ok(ready)) {
                done = true;
            } else {
                const common::Span<char> space{common::Span<char>{plain_}.subspan(plain_end_)};
                const common::Result<std::size_t> count{read_bytes(space)};
                if (!count.has_value()) {
                    done = true;
                } else if (count.value() == 0U) {
                    end_of_input_ = true;
                } else {
                    plain_end_ += count.value();
                }
            }
        }
    }
    if (event != InputEvent::kLine) {
        line_ = common::Span<const char>{};
    }
    return event;
}

InputEvent LineInput::take_stop() noexcept {
    const std::int32_t signal{take_stop_request(cancel_descriptor_)};
    // Nothing to take from a readable descriptor would make the wait return again at once.
    InputEvent event{InputEvent::kFailed};
    if (ends_program(signal)) {
        signal_ = signal;
        event = InputEvent::kStopped;
    } else if (signal != 0) {
        event = InputEvent::kCancelled;
    } else {
        // kFailed.
    }
    return event;
}

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

void LineInput::draw(const std::string_view prompt) noexcept {
    const std::size_t width{columns(out_->descriptor())};
    const common::Span<const char> text{editor_.line()};
    const std::size_t cursor{editor_.cursor()};
    // Columns for the line after the prompt, keeping the last column free for the cursor.
    const std::size_t room{(width > (prompt.size() + 1U)) ? (width - prompt.size() - 1U) : 1U};
    const std::size_t first{(cursor > room) ? (cursor - room) : 0U};
    const std::size_t shown{((text.size() - first) < room) ? (text.size() - first) : room};
    const std::size_t column{prompt.size() + (cursor - first)};
    Output output{};
    bool fits{output.append('\r') && output.append(prompt) && output.append(text.subspan(first, shown)) &&
              output.append(kClearToEnd) && output.append('\r')};
    if (fits && (column > 0U)) {
        fits = output.append(kCursorRight) && common::append_unsigned(output, column) && output.append('C');
    }
    if (fits) {
        out_->write(output.text());
    }
}

void LineInput::list_matches() noexcept {
    Line line{};
    for (const std::string_view match : editor_.matches()) {
        line.text((line.size() > 0U) ? "  " : "").text(match);
    }
    out_->write(span_of("\n"));
    out_->print(line);
}

}  // namespace cli
}  // namespace elm327
