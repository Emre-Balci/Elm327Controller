/// @file line_editor.hpp
/// @brief Editing one line of input: the keys of the shell, apart from the terminal.
///
/// A terminal in raw mode sends one byte per key, or an escape sequence for keys such as the arrows
/// ("\x1b[A" for Up). LineEditor turns these bytes into a line of text with a cursor, a history and
/// completion, and tells what the caller has to do after each byte (draw the line, accept it, ...).
/// It does not read or write the terminal itself: LineInput does that, and the tests drive the
/// editor byte by byte.
///
/// The keys follow GNU Readline's defaults: the arrows, Home, End and Delete; Ctrl+A, Ctrl+E,
/// Ctrl+B, Ctrl+F (start, end, left, right), Alt+B, Alt+F, Ctrl+Left, Ctrl+Right (a word left and
/// right), Ctrl+P, Ctrl+N (history), Backspace, Ctrl+D (delete), Ctrl+K, Ctrl+U, Ctrl+W (delete to
/// the end, to the start, the word before), Ctrl+L (clear the screen), Tab (complete), Ctrl+C
/// (discard the line) and Ctrl+D on an empty line (end of input). Only ASCII text is edited; other
/// bytes are ignored.

#ifndef ELM327_CLI_LINE_EDITOR_HPP
#define ELM327_CLI_LINE_EDITOR_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/containers/static_vector.hpp"

namespace elm327 {
namespace cli {

class LineEditor final {
public:
    static constexpr std::size_t kMaxLineLength{256U};
    static constexpr std::size_t kHistorySize{64U};
    static constexpr std::size_t kMaxCandidates{32U};

    using Text = common::StaticString<kMaxLineLength>;
    using Candidates = common::StaticVector<std::string_view, kMaxCandidates>;

    /// Adds to @p candidates the words that may stand where the word at the cursor is. @p before
    /// is the text of the line before that word, e.g. "set " when a setting is completed. The
    /// words must stay valid as long as the editor (e.g. string literals).
    using CompleteFunction = void (*)(common::Span<const char> before, Candidates& candidates,
                                      void* context) noexcept;

    /// What the caller does after feed().
    enum class Action : std::uint8_t {
        kNone,         ///< Nothing changed, e.g. in the middle of an escape sequence.
        kRedraw,       ///< The line or the cursor changed: draw them.
        kAccept,       ///< Enter: line() is complete.
        kCancel,       ///< Ctrl+C: the line is discarded.
        kEnd,          ///< Ctrl+D on an empty line: the end of the input.
        kClearScreen,  ///< Ctrl+L: clear the screen and draw the line.
        kList,         ///< Tab with several completions and nothing more in common: show matches().
        kBell,         ///< The key does nothing here, e.g. Left at the start of the line.
    };

    LineEditor() noexcept = default;

    /// Starts a new, empty line.
    void start() noexcept;

    /// Handles @p byte, the next byte from the terminal. @return what to do next.
    [[nodiscard]] Action feed(char byte) noexcept;

    /// The text of the line, valid until the next feed() or start().
    [[nodiscard]] common::Span<const char> line() const noexcept {
        return line_.text();
    }

    /// The position of the cursor in line(), from 0 to line().size().
    [[nodiscard]] std::size_t cursor() const noexcept {
        return cursor_;
    }

    /// Adds @p line to the history, unless it is empty or repeats the most recent entry. The
    /// oldest entry makes room once kHistorySize entries are there.
    void remember(common::Span<const char> line) noexcept;

    [[nodiscard]] std::size_t history_size() const noexcept {
        return history_count_;
    }

    /// Installs the source of the completions for Tab; nullptr removes it.
    void set_completer(const CompleteFunction function, void* const context) noexcept {
        complete_ = function;
        complete_context_ = context;
    }

    /// The completions that the last Action::kList found.
    [[nodiscard]] const Candidates& matches() const noexcept {
        return matches_;
    }

private:
    enum class Escape : std::uint8_t {
        kNone,
        kStarted,  ///< After ESC.
        kCsi,      ///< After ESC [, collecting the parameters.
        kSs3,      ///< After ESC O.
    };

    [[nodiscard]] Action key(char byte) noexcept;
    [[nodiscard]] Action escape(char byte) noexcept;
    [[nodiscard]] Action sequence(char final_byte) noexcept;
    [[nodiscard]] Action complete() noexcept;

    /// Replaces @p count characters at @p start with @p insertion and puts the cursor after it.
    /// @return false (nothing changes) if the result does not fit.
    [[nodiscard]] bool replace(std::size_t start, std::size_t count, common::Span<const char> insertion) noexcept;

    [[nodiscard]] Action move_to(std::size_t position) noexcept;
    [[nodiscard]] Action erase(std::size_t start, std::size_t end) noexcept;
    [[nodiscard]] Action browse(bool older) noexcept;

    /// The start of the word before @p position, or of the word @p position is in.
    [[nodiscard]] std::size_t word_start(std::size_t position) const noexcept;
    /// The end of the word after @p position.
    [[nodiscard]] std::size_t word_end(std::size_t position) const noexcept;

    /// The @p age-th most recent history entry, 1 for the most recent. @pre 1 <= age <= history_size()
    [[nodiscard]] const Text& entry(std::size_t age) const noexcept;

    Text line_{};
    std::size_t cursor_{0U};
    std::array<Text, kHistorySize> history_{};  ///< A ring: history_next_ is the slot of the next entry.
    std::size_t history_count_{0U};
    std::size_t history_next_{0U};
    std::size_t browsed_{0U};  ///< The age of the entry shown; 0 while the line being typed is shown.
    Text draft_{};             ///< The line being typed, while the history is browsed.
    Escape escape_{Escape::kNone};
    std::array<std::uint32_t, 2U> parameters_{};  ///< The numbers of a CSI sequence, e.g. 1 and 5 in ESC [ 1 ; 5 C.
    std::size_t parameter_{0U};                    ///< The number being collected.
    CompleteFunction complete_{nullptr};
    void* complete_context_{nullptr};
    Candidates matches_{};
};

}  // namespace cli
}  // namespace elm327

#endif  // ELM327_CLI_LINE_EDITOR_HPP
