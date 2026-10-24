#include "cli/console.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/protocol/text.hpp"

namespace elm327 {
namespace cli {
namespace {

/// Each round of the write loop writes at least one byte or restarts after EINTR.
constexpr std::uint32_t kMaxWriteRounds{1000U};

/// Ends a line that did not fit.
constexpr std::string_view kCutMark{"..."};

}  // namespace

Line& Line::text(const std::string_view value) noexcept {
    return text(common::Span<const char>{value.data(), value.size()});
}

Line& Line::text(const common::Span<const char> value) noexcept {
    keep(text_.append(value));
    return *this;
}

Line& Line::character(const char value) noexcept {
    keep(text_.append(value));
    return *this;
}

Line& Line::number(const std::uint64_t value) noexcept {
    keep(common::append_unsigned(text_, value));
    return *this;
}

Line& Line::hex(const std::uint64_t value, const std::size_t digits) noexcept {
    keep(common::append_hex(text_, value, digits));
    return *this;
}

Line& Line::hex_bytes(const common::Span<const std::uint8_t> bytes) noexcept {
    keep(common::append_hex_bytes(text_, bytes, ' '));
    return *this;
}

Line& Line::pad(const std::size_t column) noexcept {
    for (std::size_t size{text_.size()}; (!cut_) && (size < column); ++size) {
        keep(text_.append(' '));
    }
    return *this;
}

Line& Line::right(const common::Span<const char> value, const std::size_t width) noexcept {
    const std::size_t padding{(value.size() < width) ? (width - value.size()) : 0U};
    return pad(text_.size() + padding).text(value);
}

void Console::print(const Line& line) noexcept {
    common::StaticString<Line::kCapacity + kCutMark.size() + 1U> buffer{};
    // Cannot fail: the buffer has room for the line, the mark and the newline.
    static_cast<void>(buffer.append(line.view()));
    if (line.cut()) {
        static_cast<void>(buffer.append(kCutMark));
    }
    static_cast<void>(buffer.append('\n'));
    write(buffer.text());
}

void Console::print(const std::string_view text) noexcept {
    print(Line{}.text(text));
}

void Console::write(const common::Span<const char> text) noexcept {
    std::size_t written{0U};
    for (std::uint32_t round{0U}; (!failed_) && (written < text.size()) && (round < kMaxWriteRounds); ++round) {
        const common::Span<const char> rest{text.subspan(written)};
        const ssize_t count{::write(descriptor_, rest.data(), rest.size())};
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if ((count < 0) && (errno == EINTR)) {
            // Interrupted before anything was written: write again.
        } else {
            failed_ = true;
        }
    }
    failed_ = failed_ || (written < text.size());
}

}  // namespace cli
}  // namespace elm327
