#include "elm327/obd/service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/protocol/text.hpp"

namespace elm327 {
namespace {

/// @return true if @p command starts with "AT" in either case: an adapter command.
bool is_at_command(const std::string_view command) noexcept {
    return (command.size() >= 2U) && ((command[0U] == 'A') || (command[0U] == 'a')) &&
           ((command[1U] == 'T') || (command[1U] == 't'));
}

/// The first byte of @p request, skipping white space, e.g. 0x04 for "04", "0 4" or " 04 0C".
/// @return kUnknown-worthy false when there are not two hexadecimal digits before any other
///         character; otherwise the byte in @p service.
bool first_service_byte(const std::string_view request, std::uint8_t& service) noexcept {
    std::array<char, 2U> digits{};
    std::size_t found{0U};
    bool ok{true};
    for (std::size_t index{0U}; ok && (found < 2U) && (index < request.size()); ++index) {
        const char character{request[index]};
        if (common::is_space(character)) {
            continue;  // The adapter ignores spaces inside a request.
        }
        if (!common::is_hex_digit(character)) {
            ok = false;  // A non-hexadecimal character before a full byte: not an OBD/UDS request.
        } else {
            digits[found] = character;
            ++found;
        }
    }
    const bool complete{ok && (found == 2U)};
    if (complete) {
        // Cannot fail: two characters that is_hex_digit() accepted always fit in a std::uint8_t.
        service = common::parse_hex<std::uint8_t>(common::Span<const char>{digits.data(), digits.size()}).value_or(0U);
    }
    return complete;
}

}  // namespace

Access obd_service_access(const std::uint8_t service) noexcept {
    Access access{Access::kUnknown};
    switch (service) {
        case 0x01U:  // Current data.
        case 0x02U:  // Freeze-frame data.
        case 0x03U:  // Stored DTCs.
        case 0x05U:  // O2 sensor monitoring test results.
        case 0x06U:  // On-board monitoring test results.
        case 0x07U:  // Pending DTCs.
        case 0x09U:  // Vehicle information (VIN, calibration IDs, ...).
        case 0x0AU:  // Permanent DTCs.
            access = Access::kRead;
            break;
        case 0x04U:  // Clear the DTCs and the MIL.
        case 0x08U:  // Control of an on-board system, test or component (an actuator test).
            access = Access::kWrite;
            break;
        default:
            access = Access::kUnknown;
            break;
    }
    return access;
}

Access uds_service_access(const std::uint8_t service) noexcept {
    Access access{Access::kUnknown};
    switch (service) {
        // Reads: they only return data from the ECU.
        case 0x17U:  // KWP2000 ReadStatusOfDTC.
        case 0x18U:  // KWP2000 ReadDTCByStatus.
        case 0x19U:  // UDS ReadDTCInformation.
        case 0x1AU:  // KWP2000 ReadEcuIdentification.
        case 0x21U:  // KWP2000 ReadDataByLocalIdentifier.
        case 0x22U:  // ReadDataByIdentifier.
        case 0x23U:  // ReadMemoryByAddress.
        case 0x24U:  // ReadScalingDataByIdentifier.
        case 0x2AU:  // ReadDataByPeriodicIdentifier.
        case 0x3EU:  // TesterPresent: a keep-alive that changes nothing.
            access = Access::kRead;
            break;
        // Writes: they change the ECU, actuate it, unlock it, reset it or program it.
        case 0x10U:  // DiagnosticSessionControl: leaves the default session.
        case 0x11U:  // ECUReset.
        case 0x14U:  // ClearDiagnosticInformation.
        case 0x27U:  // SecurityAccess: unlocks the ECU for writing (seed and key).
        case 0x28U:  // CommunicationControl.
        case 0x2CU:  // DynamicallyDefineDataIdentifier.
        case 0x2EU:  // WriteDataByIdentifier (coding, adaptation).
        case 0x2FU:  // InputOutputControlByIdentifier (force an output).
        case 0x30U:  // KWP2000 InputOutputControlByLocalIdentifier.
        case 0x31U:  // RoutineControl (run a routine, e.g. an actuator or a reset).
        case 0x34U:  // RequestDownload.
        case 0x35U:  // RequestUpload.
        case 0x36U:  // TransferData.
        case 0x37U:  // RequestTransferExit.
        case 0x38U:  // RequestFileTransfer.
        case 0x3BU:  // KWP2000 WriteDataByLocalIdentifier.
        case 0x3DU:  // WriteMemoryByAddress.
        case 0x83U:  // AccessTimingParameter.
        case 0x84U:  // SecuredDataTransmission.
        case 0x85U:  // ControlDTCSetting.
        case 0x86U:  // ResponseOnEvent.
        case 0x87U:  // LinkControl.
            access = Access::kWrite;
            break;
        default:
            access = Access::kUnknown;
            break;
    }
    return access;
}

Access service_access(const std::uint8_t service) noexcept {
    const Access as_obd{obd_service_access(service)};
    const Access as_uds{uds_service_access(service)};
    Access access{Access::kUnknown};
    if ((as_obd == Access::kWrite) || (as_uds == Access::kWrite)) {
        access = Access::kWrite;  // A write in any protocol wins: never treat it as a safe read.
    } else if ((as_obd == Access::kRead) || (as_uds == Access::kRead)) {
        access = Access::kRead;
    } else {
        access = Access::kUnknown;
    }
    return access;
}

Access command_access(const std::string_view command) noexcept {
    const common::Span<const char> trimmed{common::trim(common::Span<const char>{command.data(), command.size()})};
    const std::string_view request{trimmed.data(), trimmed.size()};
    Access access{Access::kUnknown};
    std::uint8_t service{0U};
    if (is_at_command(request)) {
        access = Access::kRead;  // Configures the adapter, not the vehicle.
    } else if (first_service_byte(request, service)) {
        access = service_access(service);
    } else {
        access = Access::kUnknown;  // Empty, white space only, or not a hexadecimal request.
    }
    return access;
}

bool command_blocked(const std::string_view command, const AccessPolicy policy) noexcept {
    return (policy == AccessPolicy::kReadOnly) && (command_access(command) != Access::kRead);
}

const char* to_string(const Access access) noexcept {
    const char* text{"unknown"};
    switch (access) {
        case Access::kRead:
            text = "read";
            break;
        case Access::kWrite:
            text = "write";
            break;
        case Access::kUnknown:
            text = "unknown";
            break;
    }
    return text;
}

}  // namespace elm327
