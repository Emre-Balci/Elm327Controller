#include "elm327/expert/uds.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/protocol/text.hpp"

namespace elm327 {
namespace expert {
namespace {

constexpr std::uint8_t kNegativeResponse{0x7FU};
constexpr std::uint8_t kPositiveOffset{0x40U};
constexpr std::uint8_t kResponsePending{0x78U};
constexpr std::size_t kNegativeReplyLength{3U};  ///< "7F <service> <nrc>".

/// Clears @p out on failure so a false return always leaves it empty.
bool finish(const bool ok, UdsCommand& out) noexcept {
    if (!ok) {
        out.clear();
    }
    return ok;
}

}  // namespace

bool uds_request(const std::uint8_t service, const common::Span<const std::uint8_t> payload,
                 UdsCommand& out) noexcept {
    out.clear();
    bool ok{common::append_hex(out, service, std::size_t{2U})};
    ok = ok && common::append_hex_bytes(out, payload, '\0');
    return finish(ok, out);
}

bool uds_session_control(const DiagnosticSession session, UdsCommand& out) noexcept {
    const std::array<std::uint8_t, 1U> payload{{static_cast<std::uint8_t>(session)}};
    return uds_request(service_id(UdsService::kDiagnosticSessionControl), payload, out);
}

bool uds_tester_present(UdsCommand& out) noexcept {
    // Subfunction 0x00 with the suppress-positive-response bit (0x80): the ECU stays silent.
    const std::array<std::uint8_t, 1U> payload{{0x80U}};
    return uds_request(service_id(UdsService::kTesterPresent), payload, out);
}

bool uds_ecu_reset(const EcuReset type, UdsCommand& out) noexcept {
    const std::array<std::uint8_t, 1U> payload{{static_cast<std::uint8_t>(type)}};
    return uds_request(service_id(UdsService::kEcuReset), payload, out);
}

bool uds_read_data_by_identifier(const std::uint16_t did, UdsCommand& out) noexcept {
    const std::array<std::uint8_t, 2U> payload{
        {static_cast<std::uint8_t>(did >> 8U), static_cast<std::uint8_t>(did & 0xFFU)}};
    return uds_request(service_id(UdsService::kReadDataByIdentifier), payload, out);
}

bool uds_write_data_by_identifier(const std::uint16_t did, const common::Span<const std::uint8_t> value,
                                  UdsCommand& out) noexcept {
    out.clear();
    bool ok{common::append_hex(out, service_id(UdsService::kWriteDataByIdentifier), std::size_t{2U})};
    ok = ok && common::append_hex(out, did, std::size_t{4U});
    ok = ok && common::append_hex_bytes(out, value, '\0');
    return finish(ok, out);
}

bool uds_routine_control(const RoutineControlType type, const std::uint16_t routine,
                         const common::Span<const std::uint8_t> data, UdsCommand& out) noexcept {
    out.clear();
    bool ok{common::append_hex(out, service_id(UdsService::kRoutineControl), std::size_t{2U})};
    ok = ok && common::append_hex(out, static_cast<std::uint8_t>(type), std::size_t{2U});
    ok = ok && common::append_hex(out, routine, std::size_t{4U});
    ok = ok && common::append_hex_bytes(out, data, '\0');
    return finish(ok, out);
}

bool uds_security_request_seed(const std::uint8_t level, UdsCommand& out) noexcept {
    const std::array<std::uint8_t, 1U> payload{{level}};
    return uds_request(service_id(UdsService::kSecurityAccess), payload, out);
}

bool uds_security_send_key(const std::uint8_t level, const common::Span<const std::uint8_t> key,
                           UdsCommand& out) noexcept {
    out.clear();
    bool ok{common::append_hex(out, service_id(UdsService::kSecurityAccess), std::size_t{2U})};
    ok = ok && common::append_hex(out, level, std::size_t{2U});
    ok = ok && common::append_hex_bytes(out, key, '\0');
    return finish(ok, out);
}

common::Result<UdsReply> parse_uds_reply(const common::Span<const std::uint8_t> message) noexcept {
    using ReplyResult = common::Result<UdsReply>;
    ReplyResult result{ReplyResult::failure(common::Status::kInvalidArgument)};
    if (!message.empty()) {
        UdsReply reply{};
        if (message[0U] == kNegativeResponse) {
            if (message.size() >= kNegativeReplyLength) {
                reply.positive = false;
                reply.service = message[1U];
                reply.nrc = message[2U];
                reply.pending = (reply.nrc == kResponsePending);
                result = ReplyResult::success(reply);
            }
            // A "7F" with fewer than three bytes stays kInvalidArgument.
        } else {
            reply.positive = true;
            reply.service =
                (message[0U] >= kPositiveOffset) ? static_cast<std::uint8_t>(message[0U] - kPositiveOffset) : message[0U];
            reply.data = message.subspan(1U);
            result = ReplyResult::success(reply);
        }
    }
    return result;
}

const char* uds_service_name(const std::uint8_t service) noexcept {
    const char* name{"unknown service"};
    switch (service) {
        case 0x10U:
            name = "DiagnosticSessionControl";
            break;
        case 0x11U:
            name = "ECUReset";
            break;
        case 0x14U:
            name = "ClearDiagnosticInformation";
            break;
        case 0x19U:
            name = "ReadDTCInformation";
            break;
        case 0x22U:
            name = "ReadDataByIdentifier";
            break;
        case 0x23U:
            name = "ReadMemoryByAddress";
            break;
        case 0x27U:
            name = "SecurityAccess";
            break;
        case 0x28U:
            name = "CommunicationControl";
            break;
        case 0x2EU:
            name = "WriteDataByIdentifier";
            break;
        case 0x2FU:
            name = "InputOutputControlByIdentifier";
            break;
        case 0x31U:
            name = "RoutineControl";
            break;
        case 0x34U:
            name = "RequestDownload";
            break;
        case 0x36U:
            name = "TransferData";
            break;
        case 0x37U:
            name = "RequestTransferExit";
            break;
        case 0x3DU:
            name = "WriteMemoryByAddress";
            break;
        case 0x3EU:
            name = "TesterPresent";
            break;
        case 0x85U:
            name = "ControlDTCSetting";
            break;
        default:
            name = "unknown service";
            break;
    }
    return name;
}

const char* nrc_name(const std::uint8_t nrc) noexcept {
    const char* name{"manufacturer-specific / reserved"};
    switch (nrc) {
        case 0x10U:
            name = "generalReject";
            break;
        case 0x11U:
            name = "serviceNotSupported";
            break;
        case 0x12U:
            name = "subFunctionNotSupported";
            break;
        case 0x13U:
            name = "incorrectMessageLengthOrInvalidFormat";
            break;
        case 0x14U:
            name = "responseTooLong";
            break;
        case 0x21U:
            name = "busyRepeatRequest";
            break;
        case 0x22U:
            name = "conditionsNotCorrect";
            break;
        case 0x24U:
            name = "requestSequenceError";
            break;
        case 0x31U:
            name = "requestOutOfRange";
            break;
        case 0x33U:
            name = "securityAccessDenied";
            break;
        case 0x35U:
            name = "invalidKey";
            break;
        case 0x36U:
            name = "exceedNumberOfAttempts";
            break;
        case 0x37U:
            name = "requiredTimeDelayNotExpired";
            break;
        case 0x70U:
            name = "uploadDownloadNotAccepted";
            break;
        case 0x72U:
            name = "generalProgrammingFailure";
            break;
        case 0x78U:
            name = "requestCorrectlyReceived-ResponsePending";
            break;
        case 0x7EU:
            name = "subFunctionNotSupportedInActiveSession";
            break;
        case 0x7FU:
            name = "serviceNotSupportedInActiveSession";
            break;
        case 0x92U:
            name = "voltageTooHigh";
            break;
        case 0x93U:
            name = "voltageTooLow";
            break;
        default:
            name = "manufacturer-specific / reserved";
            break;
    }
    return name;
}

}  // namespace expert
}  // namespace elm327
