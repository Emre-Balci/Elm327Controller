#include <array>
#include <string>
#include <string_view>

#include "cli/line_editor.hpp"
#include "common/containers/span.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using elm327::cli::LineEditor;
using Action = LineEditor::Action;

namespace {

/// Feeds the bytes of @p keys. @return the action of the last byte.
Action type(LineEditor& editor, const std::string_view keys) {
    Action action{Action::kNone};
    for (const char key : keys) {
        action = editor.feed(key);
    }
    return action;
}

std::string line_of(const LineEditor& editor) {
    return test::string_of(editor.line());
}

constexpr std::array<std::string_view, 5U> kWords{{"info", "monitor", "dtc", "disconnect", "set"}};
constexpr std::array<std::string_view, 3U> kSettings{{"device", "baud", "verbose"}};

/// The commands as the first word, the settings after "set ".
void complete(const common::Span<const char> before, LineEditor::Candidates& candidates, void* /*context*/) noexcept {
    const std::string text{test::string_of(before)};
    if (text.empty()) {
        for (const std::string_view word : kWords) {
            static_cast<void>(candidates.push_back(word));
        }
    } else if (text == "set ") {
        for (const std::string_view word : kSettings) {
            static_cast<void>(candidates.push_back(word));
        }
    } else {
        // Nothing.
    }
}

}  // namespace

TEST_CASE(line_editor_typing_and_moving) {
    LineEditor editor{};
    editor.start();
    CHECK(type(editor, "read 0C") == Action::kRedraw);
    CHECK(line_of(editor) == "read 0C");
    CHECK(editor.cursor() == 7U);

    // Left twice, insert, Home, End.
    CHECK(type(editor, "\x1b[D\x1b[D") == Action::kRedraw);
    CHECK(editor.cursor() == 5U);
    CHECK(type(editor, "0D ") == Action::kRedraw);
    CHECK(line_of(editor) == "read 0D 0C");
    CHECK(type(editor, "\x01") == Action::kRedraw);  // Ctrl+A
    CHECK(editor.cursor() == 0U);
    CHECK(type(editor, "\x01") == Action::kNone);  // Already there.
    CHECK(type(editor, "\x1b[D") == Action::kBell);
    CHECK(type(editor, "\x1b[F") == Action::kRedraw);  // End
    CHECK(editor.cursor() == 10U);
    CHECK(type(editor, "\x1b[H") == Action::kRedraw);  // Home
    CHECK(type(editor, "\x1b[4~") == Action::kRedraw);  // End, VT220 style.
    CHECK(editor.cursor() == 10U);

    // Words: Ctrl+Left, Alt+B, Ctrl+Right, Alt+F.
    CHECK(type(editor, "\x1b[1;5D") == Action::kRedraw);
    CHECK(editor.cursor() == 8U);
    CHECK(type(editor, "\x1b" "b") == Action::kRedraw);
    CHECK(editor.cursor() == 5U);
    CHECK(type(editor, "\x1b[1;5C") == Action::kRedraw);
    CHECK(editor.cursor() == 7U);
    CHECK(type(editor, "\x1b" "f") == Action::kRedraw);
    CHECK(editor.cursor() == 10U);

    CHECK(type(editor, "\r") == Action::kAccept);
    CHECK(line_of(editor) == "read 0D 0C");

    // Bytes that are not ASCII text, and unknown keys, change nothing.
    editor.start();
    CHECK(type(editor, "\xc3\xa9\x1b[15~\x07") == Action::kNone);
    CHECK(line_of(editor).empty());
}

TEST_CASE(line_editor_deleting) {
    LineEditor editor{};
    editor.start();
    static_cast<void>(type(editor, "monitor 0C 0D"));
    CHECK(type(editor, "\x7f") == Action::kRedraw);  // Backspace
    CHECK(line_of(editor) == "monitor 0C 0");
    CHECK(type(editor, "\x17") == Action::kRedraw);  // Ctrl+W
    CHECK(line_of(editor) == "monitor 0C ");
    CHECK(type(editor, "\x17") == Action::kRedraw);
    CHECK(line_of(editor) == "monitor ");
    CHECK(type(editor, "\x01\x1b[3~") == Action::kRedraw);  // Home, Delete
    CHECK(line_of(editor) == "onitor ");
    CHECK(type(editor, "\x04") == Action::kRedraw);  // Ctrl+D deletes when the line is not empty.
    CHECK(line_of(editor) == "nitor ");
    CHECK(type(editor, "\x06\x06\x0b") == Action::kRedraw);  // Right twice, Ctrl+K
    CHECK(line_of(editor) == "ni");
    CHECK(type(editor, "\x15") == Action::kRedraw);  // Ctrl+U
    CHECK(line_of(editor).empty());
    CHECK(type(editor, "\x7f") == Action::kBell);
    CHECK(type(editor, "\x04") == Action::kEnd);  // Ctrl+D on an empty line.

    editor.start();
    static_cast<void>(type(editor, "half typed"));
    CHECK(type(editor, "\x03") == Action::kCancel);
    CHECK(type(editor, "\x0c") == Action::kClearScreen);
}

TEST_CASE(line_editor_history) {
    LineEditor editor{};
    editor.remember(test::span_of("info"));
    editor.remember(test::span_of("vin"));
    editor.remember(test::span_of("vin"));  // A repeat is not kept twice.
    editor.remember(test::span_of("   "));  // Nor an empty line.
    CHECK(editor.history_size() == 2U);

    editor.start();
    static_cast<void>(type(editor, "dt"));
    CHECK(type(editor, "\x1b[A") == Action::kRedraw);  // Up
    CHECK(line_of(editor) == "vin");
    CHECK(editor.cursor() == 3U);
    CHECK(type(editor, "\x10") == Action::kRedraw);  // Ctrl+P
    CHECK(line_of(editor) == "info");
    CHECK(type(editor, "\x1b[A") == Action::kBell);  // The oldest entry.
    CHECK(type(editor, "\x1bOB") == Action::kRedraw);  // Down, application mode.
    CHECK(line_of(editor) == "vin");
    CHECK(type(editor, "\x0e") == Action::kRedraw);  // Ctrl+N: back to the line being typed.
    CHECK(line_of(editor) == "dt");
    CHECK(type(editor, "\x1b[B") == Action::kBell);

    // The oldest entries make room for new ones.
    for (std::size_t index{0U}; index < (LineEditor::kHistorySize + 5U); ++index) {
        editor.remember(test::span_of("read " + std::to_string(index)));
    }
    CHECK(editor.history_size() == LineEditor::kHistorySize);
    editor.start();
    CHECK(type(editor, "\x1b[A") == Action::kRedraw);
    CHECK(line_of(editor) == "read " + std::to_string(LineEditor::kHistorySize + 4U));
}

TEST_CASE(line_editor_completion) {
    LineEditor editor{};
    editor.start();
    CHECK(type(editor, "\t") == Action::kBell);  // No completer.

    editor.set_completer(&complete, nullptr);
    static_cast<void>(type(editor, "mo"));
    CHECK(type(editor, "\t") == Action::kRedraw);
    CHECK(line_of(editor) == "monitor ");

    // Several matches: their common start, then the list.
    editor.start();
    static_cast<void>(type(editor, "d"));
    CHECK(type(editor, "\t") == Action::kList);
    REQUIRE(editor.matches().size() == 2U);
    CHECK(editor.matches()[0U] == "dtc");
    CHECK(editor.matches()[1U] == "disconnect");
    static_cast<void>(type(editor, "i"));
    CHECK(type(editor, "\t") == Action::kRedraw);
    CHECK(line_of(editor) == "disconnect ");

    editor.start();
    static_cast<void>(type(editor, "set v"));
    CHECK(type(editor, "\t") == Action::kRedraw);
    CHECK(line_of(editor) == "set verbose ");
    CHECK(type(editor, "\t") == Action::kBell);  // Nothing completes a value here.

    // Completion in the middle of the line keeps an existing space.
    editor.start();
    static_cast<void>(type(editor, "in 0C\x01\x06\x06"));
    CHECK(type(editor, "\t") == Action::kRedraw);
    CHECK(line_of(editor) == "info 0C");
    CHECK(editor.cursor() == 4U);
}

TEST_CASE(line_editor_full_line) {
    LineEditor editor{};
    editor.start();
    const std::string long_line(LineEditor::kMaxLineLength, 'x');
    CHECK(type(editor, long_line) == Action::kRedraw);
    CHECK(type(editor, "y") == Action::kBell);
    CHECK(line_of(editor) == long_line);
}
