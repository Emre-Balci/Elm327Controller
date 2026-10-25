/// @file uds.hpp
/// @brief Unified Diagnostic Services (ISO 14229) and KWP2000 (ISO 14230) for expert mode.
///
/// Beyond the standard OBD-II services (SAE J1979), manufacturers reach their ECUs with UDS or the
/// older KWP2000: the same request/response shape on which VCDS and OBDeleven (VAG), ddt4all
/// (Renault) and BimmerCode (BMW) build. A request is a service identifier and its data; a positive
/// reply echoes the service plus 0x40, and a negative reply is "7F <service> <NRC>", where the
/// negative response code (NRC) says why.
///
/// This header is the protocol, not a database of any car. It builds the request bytes as the
/// hexadecimal string an ELM327 expects (send it with Controller::execute()), and it reads a reply
/// back into a UdsReply. The ECU-specific knowledge -- which identifier holds which coding, what a
/// value means, the security algorithm of a given module -- lives with the vehicle, not here, and
/// the caller supplies it. In particular SecurityAccess is only framed (request a seed, send a
/// key): the seed-to-key algorithm is a manufacturer secret and is never shipped; pass the key the
/// owner's own tooling computed.
///
/// These are pure functions: they build and parse bytes and do no I/O. The read-only gate still
/// applies to whatever is sent -- uds_service_access() (in obd/service.hpp) classifies every
/// service, and a Controller in read-only mode refuses the ones that write.

#ifndef ELM327_EXPERT_UDS_HPP
#define ELM327_EXPERT_UDS_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/core/status.hpp"

namespace elm327 {
namespace expert {

/// UDS / KWP2000 request service identifiers, as sent by the tester. The write services change the
/// ECU; obd::service_access() classifies them so the read-only gate can refuse them.
enum class UdsService : std::uint8_t {
    kDiagnosticSessionControl = 0x10U,  ///< Enter a session (default, programming, extended).
    kEcuReset = 0x11U,                  ///< Restart the ECU.
    kClearDiagnosticInformation = 0x14U,
    kReadDtcInformation = 0x19U,
    kReadDataByIdentifier = 0x22U,          ///< Read a data identifier (DID), e.g. a coding block.
    kReadMemoryByAddress = 0x23U,
    kSecurityAccess = 0x27U,                ///< Request a seed, then send a key: unlocks writing.
    kCommunicationControl = 0x28U,
    kWriteDataByIdentifier = 0x2EU,         ///< Write a DID: coding and adaptation.
    kInputOutputControlByIdentifier = 0x2FU,///< Force an output (an actuator test).
    kRoutineControl = 0x31U,                ///< Start/stop/poll a routine, e.g. a calibration or reset.
    kRequestDownload = 0x34U,
    kTransferData = 0x36U,
    kRequestTransferExit = 0x37U,
    kWriteMemoryByAddress = 0x3DU,
    kTesterPresent = 0x3EU,                 ///< Keep the session alive; changes nothing.
    kControlDtcSetting = 0x85U,
};

/// Subfunctions of DiagnosticSessionControl (service 10).
enum class DiagnosticSession : std::uint8_t {
    kDefault = 0x01U,
    kProgramming = 0x02U,
    kExtended = 0x03U,
    kSafetySystem = 0x04U,
};

/// Subfunctions of EcuReset (service 11).
enum class EcuReset : std::uint8_t {
    kHardReset = 0x01U,
    kKeyOffOnReset = 0x02U,
    kSoftReset = 0x03U,
};

/// RoutineControl (service 31) subfunctions.
enum class RoutineControlType : std::uint8_t {
    kStart = 0x01U,
    kStop = 0x02U,
    kRequestResults = 0x03U,
};

/// The service byte of @p service.
[[nodiscard]] constexpr std::uint8_t service_id(const UdsService service) noexcept {
    return static_cast<std::uint8_t>(service);
}

/// The positive-response service byte for @p service (the request service plus 0x40).
[[nodiscard]] constexpr std::uint8_t positive_response_id(const UdsService service) noexcept {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(service) + 0x40U);
}

/// The longest request this builds, as hexadecimal text. It matches Adapter::kMaxCommandLength (64),
/// so the longest write the adapter accepts is a service, up to a two-byte identifier and the rest
/// data: 6 + 2 * kMaxRequestData hexadecimal characters.
inline constexpr std::size_t kMaxCommandText{64U};
inline constexpr std::size_t kMaxRequestData{29U};

/// A request built as the hexadecimal string an ELM327 expects, e.g. "22F190".
using UdsCommand = common::StaticString<kMaxCommandText>;

/// Builds the request "<service><payload>" in hexadecimal into @p out (cleared first).
/// @return true; false (and @p out empty) if it would exceed kMaxCommandText.
[[nodiscard]] bool uds_request(std::uint8_t service, common::Span<const std::uint8_t> payload, UdsCommand& out) noexcept;

/// DiagnosticSessionControl: "10 <session>".
[[nodiscard]] bool uds_session_control(DiagnosticSession session, UdsCommand& out) noexcept;

/// TesterPresent with the suppress-positive-response bit: "3E80". A pure keep-alive.
[[nodiscard]] bool uds_tester_present(UdsCommand& out) noexcept;

/// EcuReset: "11 <type>".
[[nodiscard]] bool uds_ecu_reset(EcuReset type, UdsCommand& out) noexcept;

/// ReadDataByIdentifier: "22 <did:2>". A read; allowed in read-only mode.
[[nodiscard]] bool uds_read_data_by_identifier(std::uint16_t did, UdsCommand& out) noexcept;

/// WriteDataByIdentifier: "2E <did:2> <value...>". A write (coding/adaptation).
[[nodiscard]] bool uds_write_data_by_identifier(std::uint16_t did, common::Span<const std::uint8_t> value,
                                                UdsCommand& out) noexcept;

/// RoutineControl: "31 <type> <routine:2> <data...>".
[[nodiscard]] bool uds_routine_control(RoutineControlType type, std::uint16_t routine,
                                       common::Span<const std::uint8_t> data, UdsCommand& out) noexcept;

/// SecurityAccess requestSeed: "27 <level>" (an odd level requests a seed). Framing only; the reply
/// carries the seed, and the key is computed by the owner's tooling, not here.
[[nodiscard]] bool uds_security_request_seed(std::uint8_t level, UdsCommand& out) noexcept;

/// SecurityAccess sendKey: "27 <level> <key...>" (an even level sends the key for the seed before).
[[nodiscard]] bool uds_security_send_key(std::uint8_t level, common::Span<const std::uint8_t> key,
                                         UdsCommand& out) noexcept;

/// A reply to a UDS/KWP request, decoded from its bytes.
struct UdsReply final {
    bool positive{false};    ///< true for "<service+0x40> ...", false for a negative reply "7F ...".
    std::uint8_t service{0U};///< The request service the reply echoes (e.g. 0x22 for a read).
    std::uint8_t nrc{0U};    ///< The negative response code when !positive; see nrc_name().
    /// The bytes after the service byte (positive) -- for a read, the identifier and its value --
    /// or empty for a negative reply. Views into @p message.
    common::Span<const std::uint8_t> data{};
    /// true when the negative reply is "requestCorrectlyReceived-ResponsePending" (NRC 0x78): the
    /// ECU is still working and another reply follows. The caller keeps waiting.
    bool pending{false};
};

/// Interprets @p message, the decoded bytes of one response (e.g. {0x62,0xF1,0x90,...} or
/// {0x7F,0x22,0x31}).
/// @return the reply; kInvalidArgument for an empty message or a malformed negative reply.
[[nodiscard]] common::Result<UdsReply> parse_uds_reply(common::Span<const std::uint8_t> message) noexcept;

/// @return a short name for the request service @p service, e.g. "ReadDataByIdentifier", or
///         "unknown service" for one this does not name.
[[nodiscard]] const char* uds_service_name(std::uint8_t service) noexcept;

/// @return the name of negative response code @p nrc, e.g. "securityAccessDenied" for 0x33, or
///         "manufacturer-specific / reserved" for one not in ISO 14229.
[[nodiscard]] const char* nrc_name(std::uint8_t nrc) noexcept;

}  // namespace expert
}  // namespace elm327

#endif  // ELM327_EXPERT_UDS_HPP
