#include "cli/line_editor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/protocol/text.hpp"

namespace elm327 {
namespace cli {
namespace {

/// The characters that can be typed into a line: printable ASCII.
constexpr std::string_view kPrintable{
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"};

/// Decimal digits, indexed by their value.
constexpr std::string_view kDigits{"0123456789"};

constexpr char kCtrlA{'\x01'};
constexpr char kCtrlB{'\x02'};
constexpr char kCtrlC{'\x03'};
constexpr char kCtrlD{'\x04'};
constexpr char kCtrlE{'\x05'};
constexpr char kCtrlF{'\x06'};
constexpr char kCtrlH{'\x08'};
constexpr char kTab{'\t'};
constexpr char kNewline{'\n'};
constexpr char kCtrlK{'\x0B'};
constexpr char kCtrlL{'\x0C'};
constexpr char kEnter{'\r'};
constexpr char kCtrlN{'\x0E'};
constexpr char kCtrlP{'\x10'};
constexpr char kCtrlU{'\x15'};
constexpr char kCtrlW{'\x17'};
constexpr char kEscape{'\x1B'};
constexpr char kBackspace{'\x7F'};

/// Largest parameter of an escape sequence that is told apart; larger ones stay at this value.
constexpr std::uint32_t kMaxParameter{9999U};

/// The second parameter of an arrow key with Ctrl, e.g. ESC [ 1 ; 5 C for Ctrl+Right.
constexpr std::uint32_t kControlModifier{5U};

/// The first parameter of ESC [ n ~ for Home, Delete and End (two codes each for Home and End).
constexpr std::uint32_t kHomeKey{1U};
constexpr std::uint32_t kDeleteKey{3U};
constexpr std::uint32_t kEndKey{4U};
constexpr std::uint32_t kHomeKeyRxvt{7U};
constexpr std::uint32_t kEndKeyRxvt{8U};

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

std::string_view view_of(const common::Span<const char> text) noexcept {
    return std::string_view{text.data(), text.size()};
}

}  // namespace

void LineEditor::start() noexcept {
    line_.clear();
    cursor_ = 0U;
    browsed_ = 0U;
    draft_.clear();
    escape_ = Escape::kNone;
    matches_.clear();
}

LineEditor::Action LineEditor::feed(const char byte) noexcept {
    return (escape_ == Escape::kNone) ? key(byte) : escape(byte);
}

void LineEditor::remember(const common::Span<const char> line) noexcept {
    const common::Span<const char> text{common::trim(line)};
    const bool repeated{(history_count_ > 0U) && common::equals(entry(1U).text(), view_of(text))};
    if ((!text.empty()) && (!repeated) && history_[history_next_].assign(view_of(text))) {
        history_next_ = (history_next_ + 1U) % kHistorySize;
        if (history_count_ < kHistorySize) {
            ++history_count_;
        }
    }
}

LineEditor::Action LineEditor::key(const char byte) noexcept {
    Action action{Action::kNone};
    if (kPrintable.find(byte) != std::string_view::npos) {
        const std::array<char, 1U> text{{byte}};
        action = replace(cursor_, 0U, text) ? Action::kRedraw : Action::kBell;
    } else if ((byte == kEnter) || (byte == kNewline)) {
        action = Action::kAccept;
    } else if (byte == kCtrlC) {
        action = Action::kCancel;
    } else if (byte == kCtrlD) {
        action = line_.empty() ? Action::kEnd : erase(cursor_, cursor_ + 1U);
    } else if ((byte == kBackspace) || (byte == kCtrlH)) {
        action = (cursor_ > 0U) ? erase(cursor_ - 1U, cursor_) : Action::kBell;
    } else if (byte == kCtrlA) {
        action = move_to(0U);
    } else if (byte == kCtrlE) {
        action = move_to(line_.size());
    } else if (byte == kCtrlB) {
        action = (cursor_ > 0U) ? move_to(cursor_ - 1U) : Action::kBell;
    } else if (byte == kCtrlF) {
        action = (cursor_ < line_.size()) ? move_to(cursor_ + 1U) : Action::kBell;
    } else if (byte == kCtrlP) {
        action = browse(true);
    } else if (byte == kCtrlN) {
        action = browse(false);
    } else if (byte == kCtrlK) {
        action = erase(cursor_, line_.size());
    } else if (byte == kCtrlU) {
        action = erase(0U, cursor_);
    } else if (byte == kCtrlW) {
        action = erase(word_start(cursor_), cursor_);
    } else if (byte == kCtrlL) {
        action = Action::kClearScreen;
    } else if (byte == kTab) {
        action = complete();
    } else if (byte == kEscape) {
        escape_ = Escape::kStarted;
        parameters_ = std::array<std::uint32_t, 2U>{};
        parameter_ = 0U;
    } else {
        // Other control characters, and bytes of other encodings, are ignored.
    }
    return action;
}

LineEditor::Action LineEditor::escape(const char byte) noexcept {
    Action action{Action::kNone};
    switch (escape_) {
        case Escape::kStarted:
            if (byte == '[') {
                escape_ = Escape::kCsi;
            } else if (byte == 'O') {
                escape_ = Escape::kSs3;
            } else {
                // Alt+B and Alt+F move by words; other Alt combinations are ignored.
                escape_ = Escape::kNone;
                if (byte == 'b') {
                    action = move_to(word_start(cursor_));
                } else if (byte == 'f') {
                    action = move_to(word_end(cursor_));
                } else {
                    // Ignored.
                }
            }
            break;
        case Escape::kCsi: {
            const std::size_t digit{kDigits.find(byte)};
            if (digit != std::string_view::npos) {
                std::uint32_t& parameter{parameters_[parameter_]};
                parameter = (parameter >= (kMaxParameter / 10U))
                                ? kMaxParameter
                                : ((parameter * 10U) + static_cast<std::uint32_t>(digit));
            } else if (byte == ';') {
                if ((parameter_ + 1U) < parameters_.size()) {
                    ++parameter_;  // Further parameters add to the last one; no key here needs them.
                }
            } else {
                escape_ = Escape::kNone;
                action = sequence(byte);
            }
            break;
        }
        case Escape::kSs3:
            escape_ = Escape::kNone;
            action = sequence(byte);
            break;
        case Escape::kNone:
            break;
    }
    return action;
}

LineEditor::Action LineEditor::sequence(const char final_byte) noexcept {
    const bool control{parameters_[1U] == kControlModifier};
    const std::uint32_t code{parameters_[0U]};
    Action action{Action::kNone};
    if (final_byte == 'A') {
        action = browse(true);
    } else if (final_byte == 'B') {
        action = browse(false);
    } else if (final_byte == 'C') {
        if (control) {
            action = move_to(word_end(cursor_));
        } else {
            action = (cursor_ < line_.size()) ? move_to(cursor_ + 1U) : Action::kBell;
        }
    } else if (final_byte == 'D') {
        if (control) {
            action = move_to(word_start(cursor_));
        } else {
            action = (cursor_ > 0U) ? move_to(cursor_ - 1U) : Action::kBell;
        }
    } else if ((final_byte == 'H') || ((final_byte == '~') && ((code == kHomeKey) || (code == kHomeKeyRxvt)))) {
        action = move_to(0U);
    } else if ((final_byte == 'F') || ((final_byte == '~') && ((code == kEndKey) || (code == kEndKeyRxvt)))) {
        action = move_to(line_.size());
    } else if ((final_byte == '~') && (code == kDeleteKey)) {
        action = erase(cursor_, cursor_ + 1U);
    } else {
        // Other keys, e.g. the function keys, are ignored.
    }
    return action;
}

LineEditor::Action LineEditor::complete() noexcept {
    matches_.clear();
    Action action{Action::kBell};
    if (complete_ != nullptr) {
        // The word at the cursor runs from the space before it to the cursor.
        std::size_t start{cursor_};
        while ((start > 0U) && (line_[start - 1U] != ' ')) {
            --start;
        }
        const common::Span<const char> text{line_.text()};
        const std::string_view prefix{view_of(text.subspan(start, cursor_ - start))};
        Candidates candidates{};
        complete_(text.first(start), candidates, complete_context_);
        for (const std::string_view candidate : candidates) {
            if (common::starts_with(span_of(candidate), prefix) && (!matches_.full())) {
                static_cast<void>(matches_.push_back(candidate));  // Cannot fail: the vector is not full.
            }
        }
        if (matches_.size() == 1U) {
            // The rest of the word, and the space before the next one.
            Text completion{};
            const bool spaced{(cursor_ < line_.size()) && (line_[cursor_] == ' ')};
            const bool fits{completion.append(span_of(matches_[0U]).subspan(prefix.size())) &&
                            (spaced || completion.append(' '))};
            action = (fits && replace(cursor_, 0U, completion.text())) ? Action::kRedraw : Action::kBell;
        } else if (matches_.size() > 1U) {
            // The characters all matches share.
            std::size_t shared{matches_[0U].size()};
            for (const std::string_view match : matches_) {
                std::size_t length{0U};
                while ((length < shared) && (length < match.size()) && (match[length] == matches_[0U][length])) {
                    ++length;
                }
                shared = length;
            }
            if (shared > prefix.size()) {
                const common::Span<const char> extra{span_of(matches_[0U]).subspan(prefix.size(), shared - prefix.size())};
                action = replace(cursor_, 0U, extra) ? Action::kRedraw : Action::kBell;
            } else {
                action = Action::kList;
            }
        } else {
            // Nothing matches: kBell.
        }
    }
    return action;
}

bool LineEditor::replace(const std::size_t start, const std::size_t count,
                         const common::Span<const char> insertion) noexcept {
    const common::Span<const char> text{line_.text()};
    Text result{};
    const bool fits{result.append(text.first(start)) && result.append(insertion) &&
                    result.append(text.subspan(start + count))};
    if (fits) {
        line_ = result;
        cursor_ = start + insertion.size();
    }
    return fits;
}

LineEditor::Action LineEditor::move_to(const std::size_t position) noexcept {
    const Action action{(position != cursor_) ? Action::kRedraw : Action::kNone};
    cursor_ = position;
    return action;
}

LineEditor::Action LineEditor::erase(const std::size_t start, const std::size_t end) noexcept {
    const std::size_t last{(end < line_.size()) ? end : line_.size()};
    Action action{Action::kBell};
    if (start < last) {
        // Cannot fail: the line only gets shorter.
        static_cast<void>(replace(start, last - start, common::Span<const char>{}));
        action = Action::kRedraw;
    }
    return action;
}

LineEditor::Action LineEditor::browse(const bool older) noexcept {
    Action action{Action::kBell};
    if (older && (browsed_ < history_count_)) {
        if (browsed_ == 0U) {
            draft_ = line_;
        }
        ++browsed_;
        line_ = entry(browsed_);
        action = Action::kRedraw;
    } else if ((!older) && (browsed_ > 0U)) {
        --browsed_;
        line_ = (browsed_ == 0U) ? draft_ : entry(browsed_);
        action = Action::kRedraw;
    } else {
        // At the end of the history: kBell.
    }
    cursor_ = (action == Action::kRedraw) ? line_.size() : cursor_;
    return action;
}

std::size_t LineEditor::word_start(const std::size_t position) const noexcept {
    std::size_t start{position};
    while ((start > 0U) && (line_[start - 1U] == ' ')) {
        --start;
    }
    while ((start > 0U) && (line_[start - 1U] != ' ')) {
        --start;
    }
    return start;
}

std::size_t LineEditor::word_end(const std::size_t position) const noexcept {
    std::size_t end{position};
    while ((end < line_.size()) && (line_[end] == ' ')) {
        ++end;
    }
    while ((end < line_.size()) && (line_[end] != ' ')) {
        ++end;
    }
    return end;
}

const LineEditor::Text& LineEditor::entry(const std::size_t age) const noexcept {
    return history_[((history_next_ + kHistorySize) - age) % kHistorySize];
}

}  // namespace cli
}  // namespace elm327
