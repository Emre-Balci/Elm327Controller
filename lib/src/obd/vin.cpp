#include "elm327/obd/vin.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "common/containers/span.hpp"

namespace elm327 {
namespace {

// The VIN bytes are viewed as characters, which is valid because std::uint8_t is unsigned char.
static_assert(std::is_same<std::uint8_t, unsigned char>::value, "std::uint8_t must be unsigned char");

/// The request: service 09, PID 02.
constexpr std::array<std::uint8_t, 2U> kRequest{{0x09U, 0x02U}};

/// Bytes before the VIN in a message: 49 02 and the item count (CAN) or message number (others).
constexpr std::size_t kHeaderSize{3U};

/// The other protocols: five numbered messages of four bytes.
constexpr std::uint32_t kLegacyMessages{5U};
constexpr std::size_t kLegacyDataSize{4U};
constexpr std::size_t kLegacyMessageSize{kHeaderSize + kLegacyDataSize};

/// The characters a VIN may hold. ISO 3779 leaves out I, O and Q, but they are accepted here, so
/// that an unusual VIN is still shown.
constexpr std::string_view kVinCharacters{"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"};

/// The bytes of the VIN, padding included, gathered from the messages.
class Payload final {
public:
    /// Appends @p data. @return false (nothing is appended) if it does not fit.
    bool append(const common::Span<const std::uint8_t> data) noexcept {
        const bool fits{data.size() <= (bytes_.size() - size_)};
        if (fits) {
            for (const std::uint8_t byte : data) {
                bytes_[size_] = byte;
                ++size_;
            }
        }
        return fits;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0U;
    }

    /// The bytes after the leading padding bytes 00, viewed as characters.
    [[nodiscard]] common::Span<const char> characters() const noexcept {
        std::size_t start{0U};
        while ((start < size_) && (bytes_[start] == 0U)) {
            ++start;
        }
        // char may access the bytes of any object (exception to MISRA C++:2023 Rule 8.2.5); the
        // characters are only compared, never converted to numbers (Rule 7.0.3).
        return common::Span<const char>{reinterpret_cast<const char*>(bytes_.data()), size_}.subspan(start);
    }

private:
    std::array<std::uint8_t, Message::kMaxSize> bytes_{};
    std::size_t size_{0U};
};

/// CAN: the first message that answers the request and is longer than the legacy form.
void collect_can(const MessageList& messages, Payload& payload) noexcept {
    for (std::size_t index{0U}; payload.empty() && (index < messages.size()); ++index) {
        const Message& message{messages[index]};
        if (answers(message, kRequest) && (message.size() > kLegacyMessageSize)) {
            static_cast<void>(payload.append(message.bytes().subspan(kHeaderSize)));
        }
    }
}

/// The other protocols: the data of the messages numbered 1 to 5, in this order.
void collect_legacy(const MessageList& messages, Payload& payload) noexcept {
    for (std::uint32_t number{1U}; number <= kLegacyMessages; ++number) {
        bool found{false};
        for (std::size_t index{0U}; (!found) && (index < messages.size()); ++index) {
            const common::Span<const std::uint8_t> bytes{messages[index].bytes()};
            found = answers(messages[index], kRequest) && (bytes.size() == kLegacyMessageSize) &&
                    (std::uint32_t{bytes[2U]} == number);
            if (found) {
                static_cast<void>(payload.append(bytes.subspan(kHeaderSize)));
            }
        }
    }
}

bool is_vin(const common::Span<const char> text) noexcept {
    bool valid{text.size() == kVinLength};
    for (std::size_t index{0U}; valid && (index < text.size()); ++index) {
        valid = kVinCharacters.find(text[index]) != std::string_view::npos;
    }
    return valid;
}

}  // namespace

common::Status decode_vin(const MessageList& messages, Vin& vin) noexcept {
    vin.clear();
    Payload payload{};
    collect_can(messages, payload);
    if (payload.empty()) {
        collect_legacy(messages, payload);
    }
    common::Status status{common::Status::kInvalidArgument};
    if (payload.empty()) {
        const common::Result<common::Span<const std::uint8_t>> response{find_response(messages, kRequest)};
        status = response.has_value() ? common::Status::kInvalidArgument : response.status();
    } else if (is_vin(payload.characters())) {
        status = vin.append(payload.characters()) ? common::Status::kOk : common::Status::kInvalidArgument;
    } else {
        // Not 17 valid characters.
    }
    return status;
}

}  // namespace elm327
