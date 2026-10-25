#include "cli/terminal.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "common/containers/static_string.hpp"

namespace elm327 {
namespace cli {
namespace {

/// Rounds of a wait for the user, each one ended by a console event that types nothing or by a look
/// into an empty pipe: a bound for the loop, not a limit anybody reaches.
constexpr std::uint32_t kMaxWaitRounds{std::numeric_limits<std::uint32_t>::max()};

/// How often wait() looks into an empty pipe, in milliseconds: a pipe cannot be waited for.
constexpr DWORD kPipePollMilliseconds{20U};

/// Console input records taken per read at most.
constexpr std::uint32_t kMaxRecords{64U};

/// What one key press types: a character in UTF-8, or an escape sequence such as ESC [ 1 ; 5 C.
using KeyText = common::StaticString<8U>;

/// The escape sequence a terminal sends for a special key, alone and with Ctrl.
struct SpecialKey final {
    WORD code;
    std::string_view text;
    std::string_view with_control;
};

/// The special keys the LineEditor knows. A console in raw mode sends their sequences itself
/// (ENABLE_VIRTUAL_TERMINAL_INPUT); one that reports the keys instead gets them from this table.
constexpr std::array<SpecialKey, 7U> kSpecialKeys{{
    {VK_UP, "\x1b[A", "\x1b[A"},
    {VK_DOWN, "\x1b[B", "\x1b[B"},
    {VK_RIGHT, "\x1b[C", "\x1b[1;5C"},
    {VK_LEFT, "\x1b[D", "\x1b[1;5D"},
    {VK_HOME, "\x1b[H", "\x1b[H"},
    {VK_END, "\x1b[F", "\x1b[F"},
    {VK_DELETE, "\x1b[3~", "\x1b[3~"},
}};

/// The text the key event @p key types: the character in UTF-8, or the escape sequence of a special
/// key. @return false, with @p text empty, for a key release and for keys that type nothing.
bool key_text(const KEY_EVENT_RECORD& key, KeyText& text) noexcept {
    text.clear();
    if (key.bKeyDown == FALSE) {
        // A release types nothing.
    } else if (key.uChar.UnicodeChar != L'\0') {
        std::array<char, 4U> bytes{};
        const int length{::WideCharToMultiByte(CP_UTF8, 0U, &key.uChar.UnicodeChar, 1, bytes.data(),
                                               static_cast<int>(bytes.size()), nullptr, nullptr)};
        if (length > 0) {
            static_cast<void>(text.append(common::Span<const char>{bytes.data(), static_cast<std::size_t>(length)}));
        }
    } else {
        const bool control{(key.dwControlKeyState & static_cast<DWORD>(LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0U};
        for (const SpecialKey& special : kSpecialKeys) {
            if (special.code == key.wVirtualKeyCode) {
                static_cast<void>(text.append(control ? special.with_control : special.text));
            }
        }
    }
    return !text.empty();
}

/// The text the console input record @p record types; see key_text().
bool record_text(const INPUT_RECORD& record, KeyText& text) noexcept {
    text.clear();
    return (record.EventType == KEY_EVENT) && key_text(record.Event.KeyEvent, text);
}

/// @return true when @p handle (nullptr for none) is signaled.
bool signaled(const HANDLE handle) noexcept {
    return (handle != nullptr) && (::WaitForSingleObject(handle, 0U) == WAIT_OBJECT_0);
}

/// Looks at the next record of the console @p input. @return true when it is a key press that types
/// something; otherwise drops it (a key release, the mouse, focus, a resized window), so that the read
/// that follows does not wait for the next key. False also when there is no record.
bool next_types(const HANDLE input) noexcept {
    INPUT_RECORD record{};
    DWORD count{0U};
    bool types{false};
    if ((::PeekConsoleInputW(input, &record, 1U, &count) != FALSE) && (count == 1U)) {
        KeyText text{};
        types = record_text(record, text);
        if (!types) {
            static_cast<void>(::ReadConsoleInputW(input, &record, 1U, &count));
        }
    }
    return types;
}

/// Reads the records waiting in the console @p input into @p buffer as the bytes a terminal sends for
/// them, each key as often as it repeated; records that type nothing are dropped. A key whose text
/// does not fit stays for the next read, unless nothing was read yet.
/// @return the number of bytes; kInterrupted when none were read (the caller waits again).
common::Result<std::size_t> read_keys(const HANDLE input, const common::Span<char> buffer) noexcept {
    std::size_t size{0U};
    bool more{true};
    common::Status status{common::Status::kOk};
    for (std::uint32_t round{0U}; is_ok(status) && more && (round < kMaxRecords); ++round) {
        DWORD available{0U};
        INPUT_RECORD record{};
        DWORD count{0U};
        if (::GetNumberOfConsoleInputEvents(input, &available) == FALSE) {
            status = common::Status::kSystemError;
        } else if ((available == 0U) || (::PeekConsoleInputW(input, &record, 1U, &count) == FALSE) ||
                   (count != 1U)) {
            more = false;
        } else {
            KeyText text{};
            const std::size_t repeat{record_text(record, text) ? std::size_t{record.Event.KeyEvent.wRepeatCount} : 0U};
            if (((text.size() * repeat) <= (buffer.size() - size)) || (size == 0U)) {
                static_cast<void>(::ReadConsoleInputW(input, &record, 1U, &count));
                for (std::size_t copy{0U}; (copy < repeat) && (text.size() <= (buffer.size() - size)); ++copy) {
                    for (const char byte : text.text()) {
                        buffer[size] = byte;
                        ++size;
                    }
                }
            } else {
                more = false;  // It stays for the next read.
            }
        }
    }
    if (is_ok(status) && (size == 0U)) {
        status = common::Status::kInterrupted;
    }
    return is_ok(status) ? common::Result<std::size_t>::success(size) : common::Result<std::size_t>::failure(status);
}

}  // namespace

LineInput::LineInput(const common::NativeHandle input, Console& out,
                     const common::NativeHandle cancel_descriptor) noexcept
    : input_{input}, out_{&out}, cancel_descriptor_{cancel_descriptor} {
    DWORD mode{0U};
    console_input_ = ::GetConsoleMode(input, &mode) != FALSE;
    pipe_input_ = (!console_input_) && (::GetFileType(input) == FILE_TYPE_PIPE);
    editing_ = console_input_ && (::GetConsoleMode(out.descriptor(), &mode) != FALSE);
}

common::Status LineInput::wait() const noexcept {
    common::Status status{common::Status::kSystemError};
    bool done{false};
    for (std::uint32_t round{0U}; (!done) && (round < kMaxWaitRounds); ++round) {
        done = true;
        if (signaled(cancel_descriptor_)) {
            status = common::Status::kInterrupted;  // A stop request wins over input, as on Linux.
        } else if (console_input_) {
            // A person takes as long as they like: no timeout.
            const std::array<HANDLE, 2U> handles{{input_, cancel_descriptor_}};
            const DWORD count{(cancel_descriptor_ != nullptr) ? 2U : 1U};
            const DWORD outcome{::WaitForMultipleObjects(count, handles.data(), FALSE, INFINITE)};
            if (outcome == (WAIT_OBJECT_0 + 1U)) {
                status = common::Status::kInterrupted;
            } else if (outcome != WAIT_OBJECT_0) {
                status = common::Status::kSystemError;
            } else if ((!raw_) || next_types(input_)) {
                status = common::Status::kOk;
            } else {
                done = false;  // An event that types nothing: wait for the next one.
            }
        } else if (pipe_input_) {
            DWORD available{0U};
            if ((::PeekNamedPipe(input_, nullptr, 0U, nullptr, &available, nullptr) == FALSE) || (available > 0U)) {
                status = common::Status::kOk;  // Input, or its end (or an error): the read tells which.
            } else {
                // Look again in a moment, unless a stop request comes first.
                if (cancel_descriptor_ != nullptr) {
                    static_cast<void>(::WaitForSingleObject(cancel_descriptor_, kPipePollMilliseconds));
                } else {
                    ::Sleep(kPipePollMilliseconds);
                }
                done = false;
            }
        } else {
            status = common::Status::kOk;  // A file is always ready.
        }
    }
    return status;
}

common::Result<std::size_t> LineInput::read_bytes(const common::Span<char> buffer) const noexcept {
    using SizeResult = common::Result<std::size_t>;
    SizeResult result{SizeResult::failure(common::Status::kSystemError)};
    if (console_input_ && raw_) {
        result = read_keys(input_, buffer);
    } else {
        DWORD count{0U};
        // A console reports a line that Ctrl+C ended as a read of nothing with this error, so it must
        // not be left over from an earlier call.
        ::SetLastError(ERROR_SUCCESS);
        const bool read{::ReadFile(input_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) != FALSE};
        const DWORD error{::GetLastError()};
        if ((error == ERROR_OPERATION_ABORTED) && (count == 0U)) {
            result = SizeResult::failure(common::Status::kInterrupted);  // The stop request follows.
        } else if (read) {
            result = SizeResult::success(count);
        } else if (error == ERROR_BROKEN_PIPE) {
            result = SizeResult::success(0U);  // The other end closed the pipe: the end of the input.
        } else {
            // kSystemError.
        }
    }
    return result;
}

common::Status LineInput::enter_raw_mode() noexcept {
    common::Status status{common::Status::kOk};
    if (!raw_) {
        DWORD input_mode{0U};
        DWORD output_mode{0U};
        status = ((::GetConsoleMode(input_, &input_mode) != FALSE) &&
                  (::GetConsoleMode(out_->descriptor(), &output_mode) != FALSE))
                     ? common::Status::kOk
                     : common::Status::kSystemError;
        if (is_ok(status)) {
            // Every key arrives at once, without echo, and Ctrl+C as a key rather than a stop request.
            // The special keys arrive as the escape sequences of a terminal.
            const DWORD cooked{static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)};
            const DWORD raw{(input_mode & (~cooked)) | static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_INPUT)};
            // The output carries out the escape sequences that draw the line, and "\n" starts a new line.
            const DWORD terminal{output_mode |
                                 static_cast<DWORD>(ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)};
            status = (::SetConsoleMode(input_, raw) != FALSE) ? common::Status::kOk : common::Status::kSystemError;
            if (is_ok(status) && (::SetConsoleMode(out_->descriptor(), terminal) == FALSE)) {
                static_cast<void>(::SetConsoleMode(input_, input_mode));
                status = common::Status::kSystemError;
            }
        }
        if (is_ok(status)) {
            saved_input_mode_ = static_cast<std::uint32_t>(input_mode);
            saved_output_mode_ = static_cast<std::uint32_t>(output_mode);
        }
        raw_ = is_ok(status);
    }
    return status;
}

void LineInput::leave_raw_mode() noexcept {
    if (raw_) {
        // Nothing better to do if the console refuses: it is gone.
        static_cast<void>(::SetConsoleMode(input_, static_cast<DWORD>(saved_input_mode_)));
        static_cast<void>(::SetConsoleMode(out_->descriptor(), static_cast<DWORD>(saved_output_mode_)));
        raw_ = false;
    }
}

std::size_t LineInput::columns() const noexcept {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    std::size_t width{kDefaultColumns};
    if (::GetConsoleScreenBufferInfo(out_->descriptor(), &info) != FALSE) {
        const std::int32_t visible{(std::int32_t{info.srWindow.Right} - std::int32_t{info.srWindow.Left}) + 1};
        if (visible > 0) {
            width = static_cast<std::size_t>(visible);
        }
    }
    return width;
}

}  // namespace cli
}  // namespace elm327
