/// @file terminal.hpp
/// @brief Reading the lines of the shell: from a terminal with line editing, or from a pipe or file.
///
/// When the input and the output are a terminal, LineInput puts the terminal into raw mode while a
/// line is typed, so it receives every key, and draws the prompt and the line itself after each
/// key (see LineEditor). Between the lines, while a command runs, the terminal is back in its
/// normal mode: Ctrl+C then sends SIGINT, which stops the command (see StopSignals).
///
/// Otherwise, e.g. with a script piped into elm327ctl, LineInput reads plain lines without a prompt.

#ifndef ELM327_CLI_TERMINAL_HPP
#define ELM327_CLI_TERMINAL_HPP

#include <termios.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cli/console.hpp"
#include "cli/line_editor.hpp"
#include "common/containers/span.hpp"
#include "common/core/status.hpp"

namespace elm327 {
namespace cli {

/// What LineInput::read() found.
enum class InputEvent : std::uint8_t {
    kLine,       ///< A line: see LineInput::line().
    kCancelled,  ///< Ctrl+C at the prompt, or SIGINT while waiting for input: no line.
    kEnd,        ///< The end of the input: Ctrl+D, or the end of a file or pipe.
    kStopped,    ///< A signal that ends the program: see LineInput::signal().
    kTooLong,    ///< A plain line longer than kMaxPlainLine; it is skipped.
    kFailed,     ///< Reading failed.
};

class LineInput final {
public:
    /// Longest line read from a pipe or a file.
    static constexpr std::size_t kMaxPlainLine{1023U};

    /// Reads from @p input. Prompts and edited lines go to @p out, which must outlive the object.
    /// Once @p cancel_descriptor is readable, read() takes the stop request (take_stop_request()).
    LineInput(std::int32_t input, Console& out, std::int32_t cancel_descriptor) noexcept;

    /// Restores the terminal if a line was being read.
    ~LineInput();

    LineInput(const LineInput&) = delete;
    LineInput& operator=(const LineInput&) = delete;
    LineInput(LineInput&&) = delete;
    LineInput& operator=(LineInput&&) = delete;

    /// @return true when the input and the output are a terminal, so lines are edited after a prompt.
    [[nodiscard]] bool editing() const noexcept {
        return editing_;
    }

    /// Reads the next line, after showing @p prompt when editing. Waits for the user as long as
    /// it takes: only the cancel descriptor and the end of the input end the wait.
    [[nodiscard]] InputEvent read(std::string_view prompt) noexcept;

    /// The line of the last InputEvent::kLine, without its end. Valid until the next read().
    [[nodiscard]] common::Span<const char> line() const noexcept {
        return line_;
    }

    /// The signal of the last InputEvent::kStopped.
    [[nodiscard]] std::int32_t signal() const noexcept {
        return signal_;
    }

    /// The editor, e.g. to install completion or to add lines to the history.
    [[nodiscard]] LineEditor& editor() noexcept {
        return editor_;
    }

private:
    /// Bytes taken from the input per read.
    static constexpr std::size_t kChunkSize{64U};

    [[nodiscard]] InputEvent read_edited(std::string_view prompt) noexcept;
    [[nodiscard]] InputEvent read_plain() noexcept;

    /// Handles a stop request on the cancel descriptor. @return kStopped for a signal that ends the
    /// program, kCancelled for SIGINT; kFailed if nothing could be taken, since the wait would
    /// return again at once.
    [[nodiscard]] InputEvent take_stop() noexcept;

    /// Waits until the input or the cancel descriptor is readable.
    /// @return kOk for input (or its end), kInterrupted for the cancel descriptor, kSystemError.
    [[nodiscard]] common::Status wait() const noexcept;

    /// Reads up to @p buffer.size() bytes. @return the number of bytes, 0 at the end of the input.
    [[nodiscard]] common::Result<std::size_t> read_bytes(common::Span<char> buffer) const noexcept;

    [[nodiscard]] common::Status enter_raw_mode() noexcept;
    void leave_raw_mode() noexcept;

    /// Draws @p prompt and the editor's line in place of the current terminal line, scrolled so
    /// that the cursor is visible.
    void draw(std::string_view prompt) noexcept;

    /// Writes the completions of the editor below the line.
    void list_matches() noexcept;

    std::int32_t input_;
    Console* out_;
    std::int32_t cancel_descriptor_;
    bool editing_;
    bool raw_{false};
    termios saved_{};  ///< The terminal settings outside of read().
    LineEditor editor_{};
    common::Span<const char> line_{};
    std::int32_t signal_{0};
    /// Edited input that arrived after the end of a line, e.g. pasted lines.
    std::array<char, kChunkSize> pending_{};
    std::size_t pending_begin_{0U};
    std::size_t pending_end_{0U};
    /// Plain input: the bytes read but not handed out yet, in [plain_begin_, plain_end_).
    std::array<char, kMaxPlainLine + 1U> plain_{};
    std::size_t plain_begin_{0U};
    std::size_t plain_end_{0U};
    bool end_of_input_{false};
    bool skipping_{false};  ///< Dropping the rest of a line that was too long.
};

}  // namespace cli
}  // namespace elm327

#endif  // ELM327_CLI_TERMINAL_HPP
