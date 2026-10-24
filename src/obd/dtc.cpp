#include "elm327/obd/dtc.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/protocol/byte_order.hpp"
#include "common/protocol/text.hpp"
#include "elm327/obd/message.hpp"

namespace elm327 {
namespace {

/// The letters of the systems, indexed by the two most significant bits of a DTC.
constexpr std::string_view kSystems{"PCBU"};

constexpr std::uint32_t kSystemShift{14U};
constexpr std::uint32_t kDigitsMask{0x3FFFU};
constexpr std::uint32_t kMilBit{0x80U};
constexpr std::uint32_t kDtcCountMask{0x7FU};

bool contains(const DtcList& dtcs, const Dtc dtc) noexcept {
    bool found{false};
    for (std::size_t index{0U}; (!found) && (index < dtcs.size()); ++index) {
        found = dtcs[index] == dtc;
    }
    return found;
}

}  // namespace

std::uint8_t request_service(const DtcKind kind) noexcept {
    std::uint8_t service{0x03U};
    switch (kind) {
        case DtcKind::kStored:
            service = 0x03U;
            break;
        case DtcKind::kPending:
            service = 0x07U;
            break;
        case DtcKind::kPermanent:
            service = 0x0AU;
            break;
    }
    return service;
}

const char* to_string(const DtcKind kind) noexcept {
    const char* text{"unknown"};
    switch (kind) {
        case DtcKind::kStored:
            text = "stored";
            break;
        case DtcKind::kPending:
            text = "pending";
            break;
        case DtcKind::kPermanent:
            text = "permanent";
            break;
    }
    return text;
}

common::Result<std::size_t> format_dtc(const Dtc dtc, const common::Span<char> out) noexcept {
    using SizeResult = common::Result<std::size_t>;
    SizeResult result{SizeResult::failure(common::Status::kOutOfRange)};
    if (out.size() >= kDtcTextSize) {
        const std::uint32_t code{dtc};
        out[0U] = kSystems[code >> kSystemShift];
        // The 14 low bits are the four digits: 0-3, then three hexadecimal digits.
        const SizeResult digits{
            common::format_hex(static_cast<std::uint16_t>(code & kDigitsMask), kDtcTextSize - 1U, out.subspan(1U))};
        result = digits.has_value() ? SizeResult::success(kDtcTextSize) : digits;
    }
    return result;
}

common::Status decode_dtcs(const common::Span<const std::uint8_t> message, const std::uint8_t service,
                           DtcList& dtcs) noexcept {
    common::Status status{((!message.empty()) && (message[0U] == response_service(service)))
                              ? common::Status::kOk
                              : common::Status::kInvalidArgument};
    common::Span<const std::uint8_t> codes{};
    if (is_ok(status)) {
        const common::Span<const std::uint8_t> data{message.subspan(1U)};
        if ((data.size() % 2U) == 1U) {
            // CAN: the number of DTCs, then the DTCs.
            const std::size_t count{data[0U]};
            status = data.in_bounds(1U, 2U * count) ? common::Status::kOk : common::Status::kInvalidArgument;
            if (is_ok(status)) {
                codes = data.subspan(1U, 2U * count);
            }
        } else {
            codes = data;  // Three DTCs, padded with 00 00.
        }
    }
    for (std::size_t offset{0U}; is_ok(status) && ((offset + 1U) < codes.size()); offset += 2U) {
        const Dtc dtc{common::read_big_endian<Dtc>(codes, offset).value_or(0U)};
        if ((dtc != 0U) && (!contains(dtcs, dtc))) {
            status = dtcs.push_back(dtc) ? common::Status::kOk : common::Status::kOutOfRange;
        }
    }
    return status;
}

common::Result<MilStatus> decode_mil_status(const common::Span<const std::uint8_t> data) noexcept {
    common::Result<MilStatus> result{common::Result<MilStatus>::failure(common::Status::kOutOfRange)};
    if (!data.empty()) {
        const std::uint32_t byte_a{data[0U]};
        MilStatus mil{};
        mil.on = (byte_a & kMilBit) != 0U;
        mil.dtc_count = static_cast<std::uint8_t>(byte_a & kDtcCountMask);
        result = common::Result<MilStatus>::success(mil);
    }
    return result;
}

}  // namespace elm327
