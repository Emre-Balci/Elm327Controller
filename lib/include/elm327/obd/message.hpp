/// @file message.hpp
/// @brief The vehicle's messages in a reply of the adapter.
///
/// With headers off (the ELM327 default), every line of a reply to an OBD request is one message
/// in hexadecimal, e.g. "41 0C 1A F8" from one ECU; when several ECUs answer, each prints a line.
/// On CAN (ISO 15765-4) a message longer than 7 bytes travels in several frames, and the adapter
/// prints its length followed by the numbered frames:
/// @code
///   014
///   0: 49 02 01 31 44 34
///   1: 47 50 30 30 52 35 35
///   2: 42 31 32 33 34 35 36
/// @endcode
/// parse_messages() turns both forms into messages, and answers() and find_response() pick the
/// responses to a request from them.

#ifndef ELM327_OBD_MESSAGE_HPP
#define ELM327_OBD_MESSAGE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/containers/static_vector.hpp"
#include "common/core/status.hpp"
#include "elm327/adapter/response.hpp"

namespace elm327 {

/// First byte of a negative response: 7F, the rejected service, and a reason code.
inline constexpr std::uint8_t kNegativeResponse{0x7FU};

/// @return the first byte of a positive response to @p service: the service + 0x40, e.g. 0x41 for
///         service 01.
[[nodiscard]] constexpr std::uint8_t response_service(const std::uint8_t service) noexcept {
    return static_cast<std::uint8_t>(static_cast<std::uint32_t>(service) + 0x40U);
}

/// The bytes of one message, e.g. 41 0C 1A F8: the response service, then the data.
class Message final {
public:
    /// Longest message. A CAN message can declare up to 4095 bytes, but OBD-II replies are far
    /// shorter: a VIN takes 20 bytes.
    static constexpr std::size_t kMaxSize{256U};

    [[nodiscard]] common::Span<const std::uint8_t> bytes() const noexcept {
        return common::Span<const std::uint8_t>{bytes_.data(), size_};
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    /// Appends @p data. @return false (nothing is appended) if it does not fit.
    [[nodiscard]] bool append(common::Span<const std::uint8_t> data) noexcept;

private:
    std::array<std::uint8_t, kMaxSize> bytes_{};
    std::size_t size_{0U};
};

/// Most messages in one reply, e.g. from several ECUs.
inline constexpr std::size_t kMaxMessages{16U};

using MessageList = common::StaticVector<Message, kMaxMessages>;

/// Converts the content lines of @p response into @p messages, which is cleared first. Progress
/// lines such as "SEARCHING..." are skipped; check Response::reply() for errors before.
/// Expects headers off (ATH0), the default after a reset.
/// @return kOk; kInvalidArgument for a line that is not hexadecimal bytes, or a multi-frame message
///         with frames out of sequence or missing; kOutOfRange for a message longer than
///         Message::kMaxSize or more than kMaxMessages messages.
[[nodiscard]] common::Status parse_messages(const Response& response, MessageList& messages) noexcept;

/// @return true if @p message is a positive response to @p request: its first byte is the
///         request's service + 0x40, and it repeats the rest of the request, e.g. the PID.
[[nodiscard]] bool answers(const Message& message, common::Span<const std::uint8_t> request) noexcept;

/// @return true if @p message is a negative response to @p service, "7F <service> <reason>".
[[nodiscard]] bool rejects(const Message& message, std::uint8_t service) noexcept;

/// The first of @p messages that answers @p request, e.g. 41 0C 1A F8 for the request 01 0C.
/// @return the message's bytes, valid as long as @p messages is unchanged; kNotSupported if an ECU
///         rejected the request (a negative response) and none answered it; kInvalidArgument if
///         no message refers to the request at all.
[[nodiscard]] common::Result<common::Span<const std::uint8_t>> find_response(
    const MessageList& messages, common::Span<const std::uint8_t> request) noexcept;

}  // namespace elm327

#endif  // ELM327_OBD_MESSAGE_HPP
