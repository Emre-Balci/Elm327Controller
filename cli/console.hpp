/// @file console.hpp
/// @brief Text output of elm327ctl, line by line, without stdio and without heap.

#ifndef ELM327_CLI_CONSOLE_HPP
#define ELM327_CLI_CONSOLE_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/core/config.hpp"

namespace elm327 {
namespace cli {

/// A line of text under construction. The appends return the line, so they chain:
/// @code
///   console.print(Line{}.text("Voltage").pad(13).text(voltage));
/// @endcode
/// An append that does not fit is dropped and marks the line as cut; Console::print() then ends it
/// with "...".
class Line final {
public:
    /// Room for a monitor row with Options::kMaxArguments columns.
    static constexpr std::size_t kCapacity{512U};

    Line& text(std::string_view value) noexcept;
    Line& text(common::Span<const char> value) noexcept;
    Line& character(char value) noexcept;

    /// @p value in decimal.
    Line& number(std::uint64_t value) noexcept;

    /// @p value as @p digits upper-case hexadecimal digits, e.g. "0C".
    Line& hex(std::uint64_t value, std::size_t digits) noexcept;

    /// @p bytes as hexadecimal pairs separated by spaces, e.g. "41 0C 1A F8".
    Line& hex_bytes(common::Span<const std::uint8_t> bytes) noexcept;

    /// Spaces up to column @p column; nothing if the line is that long already.
    Line& pad(std::size_t column) noexcept;

    /// @p value right-aligned in a field of @p width characters; a longer value is not cut.
    Line& right(common::Span<const char> value, std::size_t width) noexcept;

    [[nodiscard]] common::Span<const char> view() const noexcept {
        return text_.text();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return text_.size();
    }

    /// @return true if an append did not fit.
    [[nodiscard]] bool cut() const noexcept {
        return cut_;
    }

private:
    void keep(bool appended) noexcept {
        cut_ = cut_ || (!appended);
    }

    common::StaticString<kCapacity> text_{};
    bool cut_{false};
};

/// Writes lines to a file descriptor, e.g. STDOUT_FILENO (on Windows, a HANDLE such as the one of
/// standard_output()), with one write(2) (WriteFile()) per line: nothing is buffered, so lines
/// appear at once even in a pipe.
class Console final {
public:
    explicit Console(const common::NativeHandle descriptor) noexcept : descriptor_{descriptor} {}

    /// Writes @p line and a newline.
    void print(const Line& line) noexcept;

    /// Writes @p text and a newline.
    void print(std::string_view text) noexcept;

    /// Writes @p text as it is, without a newline, e.g. a prompt or the escape sequences of the
    /// line editor.
    void write(common::Span<const char> text) noexcept;

    /// @return true once a write failed, e.g. into a closed pipe. Later lines are not written.
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }

    [[nodiscard]] common::NativeHandle descriptor() const noexcept {
        return descriptor_;
    }

private:
    common::NativeHandle descriptor_;
    bool failed_{false};
};

/// The standard input, output and error of the process: STDIN_FILENO, STDOUT_FILENO and
/// STDERR_FILENO, or on Windows the handles GetStdHandle() returns.
[[nodiscard]] common::NativeHandle standard_input() noexcept;
[[nodiscard]] common::NativeHandle standard_output() noexcept;
[[nodiscard]] common::NativeHandle standard_error() noexcept;

}  // namespace cli
}  // namespace elm327

#endif  // ELM327_CLI_CONSOLE_HPP
