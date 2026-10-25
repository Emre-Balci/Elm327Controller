/// @file test_support.hpp
/// @brief Conversions between the library's spans and the standard types the tests compare with,
///        and what the tests need from the operating system, on Linux and Windows. The operating
///        system's headers stay in test_support.cpp: <windows.h> defines macros such as near.

#ifndef ELM327_TEST_SUPPORT_HPP
#define ELM327_TEST_SUPPORT_HPP

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "elm327/adapter/response.hpp"

namespace test {

#if defined(_WIN32)
/// A device that does not exist.
inline constexpr const char* kMissingDevice{"\\\\.\\elm327-missing"};
/// A device that exists but is not a serial port.
inline constexpr const char* kNotSerialDevice{"NUL"};
#else
inline constexpr const char* kMissingDevice{"/nonexistent/ttyUSB9"};
inline constexpr const char* kNotSerialDevice{"/dev/null"};
#endif

/// An eventfd (on Windows, a manual-reset event) that becomes readable once signal() is called: a
/// cancel descriptor for the tests.
class Event final {
public:
    Event();
    ~Event();
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    [[nodiscard]] bool valid() const {
        return descriptor_ != common::kInvalidNativeHandle;
    }

    [[nodiscard]] common::NativeHandle descriptor() const {
        return descriptor_;
    }

    void signal() const;

    /// Makes the descriptor unreadable again, as an interactive program does before it goes on.
    void clear() const;

private:
    common::NativeHandle descriptor_{common::kInvalidNativeHandle};
};

/// A file in memory (on Windows, a temporary file that is deleted when it is closed) for a Console
/// to write to; text() reads back what was written.
class Capture final {
public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) = delete;
    Capture& operator=(Capture&&) = delete;

    [[nodiscard]] bool valid() const {
        return descriptor_ != common::kInvalidNativeHandle;
    }

    [[nodiscard]] common::NativeHandle descriptor() const {
        return descriptor_;
    }

    /// Everything written so far.
    [[nodiscard]] std::string text() const;

private:
    common::NativeHandle descriptor_{common::kInvalidNativeHandle};
};

/// A pipe whose reading end gives what write_and_close() put into it and then the end of the input,
/// like a script piped into a program.
class Pipe final {
public:
    Pipe();
    ~Pipe();
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&&) = delete;
    Pipe& operator=(Pipe&&) = delete;

    [[nodiscard]] bool valid() const {
        return read_ != common::kInvalidNativeHandle;
    }

    [[nodiscard]] common::NativeHandle read_end() const {
        return read_;
    }

    /// Writes @p text, far less than the pipe holds, and closes the writing end.
    void write_and_close(const std::string& text);

private:
    common::NativeHandle read_{common::kInvalidNativeHandle};
    common::NativeHandle write_{common::kInvalidNativeHandle};
};

inline common::Span<const char> span_of(const std::string_view text) {
    return common::Span<const char>{text.data(), text.size()};
}

inline common::Span<const std::uint8_t> bytes_of(const std::vector<std::uint8_t>& bytes) {
    return common::Span<const std::uint8_t>{bytes.data(), bytes.size()};
}

inline std::string string_of(const common::Span<const char> text) {
    return std::string{text.data(), text.size()};
}

inline std::vector<std::uint8_t> vector_of(const common::Span<const std::uint8_t> bytes) {
    return std::vector<std::uint8_t>{bytes.begin(), bytes.end()};
}

/// A response with the given lines, as the adapter would have printed them.
inline elm327::Response make_response(const std::initializer_list<std::string_view> lines) {
    elm327::Response response{};
    for (const std::string_view line : lines) {
        response.add_line(span_of(line), false);
    }
    return response;
}

}  // namespace test

#endif  // ELM327_TEST_SUPPORT_HPP
