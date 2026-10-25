#include "elm327/obd/message.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/protocol/text.hpp"

namespace elm327 {
namespace {

/// Frames are numbered with one hexadecimal digit, 0 to F and on from 0.
constexpr std::uint32_t kFrameNumberMask{0x0FU};

/// A multi-frame message being reassembled.
struct Assembly final {
    std::size_t expected{0U};  ///< Declared length in bytes; 0 when no message is being reassembled.
    std::uint32_t next_frame{0U};
};

/// The length line of a multi-frame message: exactly three hexadecimal digits, e.g. "014".
bool is_length_line(const common::Span<const char> line) noexcept {
    return (line.size() == 3U) && common::is_hex_digit(line[0U]) && common::is_hex_digit(line[1U]) &&
           common::is_hex_digit(line[2U]);
}

/// A frame of a multi-frame message: its number and a colon, e.g. "1: 47 50 30 30 52 35 35".
bool is_frame_line(const common::Span<const char> line) noexcept {
    return (line.size() >= 2U) && common::is_hex_digit(line[0U]) && (line[1U] == ':');
}

/// Decodes the hexadecimal bytes of @p text and appends at most @p limit of them to @p message.
common::Status append_hex(const common::Span<const char> text, const std::size_t limit, Message& message) noexcept {
    // Without spaces, a line holds at most kMaxLineLength / 2 bytes.
    std::array<std::uint8_t, Response::kMaxLineLength / 2U> bytes{};
    const common::Result<std::size_t> count{common::decode_hex_bytes(text, bytes)};
    common::Status status{count.status()};
    if (is_ok(status)) {
        const std::size_t taken{(count.value() < limit) ? count.value() : limit};
        status = message.append(common::Span<const std::uint8_t>{bytes.data(), taken}) ? common::Status::kOk
                                                                                        : common::Status::kOutOfRange;
    }
    return status;
}

/// Starts the multi-frame message whose length line is @p line.
common::Status start_message(const common::Span<const char> line, MessageList& messages, Assembly& assembly) noexcept {
    const common::Result<std::uint16_t> length{common::parse_hex<std::uint16_t>(line)};
    common::Status status{common::Status::kInvalidArgument};
    if (length.has_value() && (length.value() > 0U)) {
        if ((length.value() <= Message::kMaxSize) && messages.emplace_back()) {
            assembly.expected = length.value();
            assembly.next_frame = 0U;
            status = common::Status::kOk;
        } else {
            status = common::Status::kOutOfRange;
        }
    }
    return status;
}

/// Adds the frame @p line to the message being reassembled, the last of @p messages.
common::Status add_frame(const common::Span<const char> line, MessageList& messages, Assembly& assembly) noexcept {
    const common::Result<std::uint8_t> number{common::hex_digit_value(line[0U])};
    common::Status status{common::Status::kInvalidArgument};
    if (number.has_value() && (std::uint32_t{number.value()} == assembly.next_frame)) {
        Message& message{messages.back()};
        // The last frame is padded to its full size; the padding is not part of the message.
        status = append_hex(line.subspan(2U), assembly.expected - message.size(), message);
        if (is_ok(status)) {
            assembly.next_frame = (assembly.next_frame + 1U) & kFrameNumberMask;
            if (message.size() == assembly.expected) {
                assembly.expected = 0U;  // Complete.
            }
        }
    }
    return status;
}

}  // namespace

bool Message::append(const common::Span<const std::uint8_t> data) noexcept {
    const bool fits{data.size() <= (kMaxSize - size_)};
    if (fits) {
        for (const std::uint8_t byte : data) {
            bytes_[size_] = byte;
            ++size_;
        }
    }
    return fits;
}

common::Status parse_messages(const Response& response, MessageList& messages) noexcept {
    messages.clear();
    common::Status status{common::Status::kOk};
    Assembly assembly{};
    for (std::size_t index{0U}; is_ok(status) && (index < response.size()); ++index) {
        const common::Span<const char> line{response.line(index)};
        const bool reassembling{assembly.expected > 0U};
        if (classify_line(line) != Reply::kContent) {
            // Progress messages such as "SEARCHING..." carry no data.
        } else if (is_length_line(line)) {
            status = reassembling ? common::Status::kInvalidArgument : start_message(line, messages, assembly);
        } else if (is_frame_line(line)) {
            status = reassembling ? add_frame(line, messages, assembly) : common::Status::kInvalidArgument;
        } else if (reassembling) {
            status = common::Status::kInvalidArgument;  // A single-line message between the frames.
        } else if (!messages.emplace_back()) {
            status = common::Status::kOutOfRange;
        } else {
            status = append_hex(line, Message::kMaxSize, messages.back());
        }
    }
    if (is_ok(status) && (assembly.expected > 0U)) {
        status = common::Status::kInvalidArgument;  // The frames of the last message are incomplete.
    }
    return status;
}

bool answers(const Message& message, const common::Span<const std::uint8_t> request) noexcept {
    const common::Span<const std::uint8_t> bytes{message.bytes()};
    bool match{(!request.empty()) && (bytes.size() >= request.size()) &&
               (bytes[0U] == response_service(request[0U]))};
    for (std::size_t index{1U}; match && (index < request.size()); ++index) {
        match = bytes[index] == request[index];
    }
    return match;
}

bool rejects(const Message& message, const std::uint8_t service) noexcept {
    const common::Span<const std::uint8_t> bytes{message.bytes()};
    return (bytes.size() >= 3U) && (bytes[0U] == kNegativeResponse) && (bytes[1U] == service);
}

common::Result<common::Span<const std::uint8_t>> find_response(
    const MessageList& messages, const common::Span<const std::uint8_t> request) noexcept {
    using SpanResult = common::Result<common::Span<const std::uint8_t>>;
    SpanResult result{SpanResult::failure(common::Status::kInvalidArgument)};
    bool found{false};
    bool rejected{false};
    for (std::size_t index{0U}; (!found) && (index < messages.size()); ++index) {
        found = answers(messages[index], request);
        if (found) {
            result = SpanResult::success(messages[index].bytes());
        } else {
            rejected = rejected || ((!request.empty()) && rejects(messages[index], request[0U]));
        }
    }
    if ((!found) && rejected) {
        result = SpanResult::failure(common::Status::kNotSupported);
    }
    return result;
}

}  // namespace elm327
