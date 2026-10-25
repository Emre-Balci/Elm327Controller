/// @file dtc.hpp
/// @brief Diagnostic trouble codes (DTCs): services 03, 07 and 0A of SAE J1979, and the MIL status.
///
/// A DTC travels as two bytes. The two most significant bits select the system (P powertrain,
/// C chassis, B body, U network), and the other 14 bits are its four digits: 01 33 is P0133.
///
/// The responses differ between the buses. On CAN (ISO 15765-4) a message holds the number of DTCs
/// and then the DTCs: "43 02 01 33 04 20". The older protocols (SAE J1850, ISO 9141-2, ISO 14230-4)
/// send three DTCs per message, padded with 00 00: "43 01 33 04 20 00 00". decode_dtcs() reads
/// both: after the service byte, the CAN form has an odd number of bytes, the others always six.

#ifndef ELM327_OBD_DTC_HPP
#define ELM327_OBD_DTC_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/containers/static_vector.hpp"
#include "common/core/status.hpp"

namespace elm327 {

/// The three lists of DTCs an ECU keeps.
enum class DtcKind : std::uint8_t {
    kStored,     ///< Service 03: confirmed codes, those that turn on the MIL.
    kPending,    ///< Service 07: detected in the current or last drive cycle, not yet confirmed.
    kPermanent,  ///< Service 0A: confirmed codes that only the ECU clears, once the fault is gone.
};

/// @return the service that requests @p kind: 0x03, 0x07 or 0x0A.
[[nodiscard]] std::uint8_t request_service(DtcKind kind) noexcept;

/// @return "stored", "pending" or "permanent".
[[nodiscard]] const char* to_string(DtcKind kind) noexcept;

/// A DTC as transmitted, e.g. 0x0133 for P0133.
using Dtc = std::uint16_t;

/// Most DTCs a list holds.
inline constexpr std::size_t kMaxDtcs{64U};

using DtcList = common::StaticVector<Dtc, kMaxDtcs>;

/// Characters of a formatted DTC, e.g. "P0133".
inline constexpr std::size_t kDtcTextSize{5U};

/// Writes the five characters of @p dtc, e.g. "P0133", to the start of @p out. No NUL is written.
/// @return kDtcTextSize; kOutOfRange if @p out is too small.
[[nodiscard]] common::Result<std::size_t> format_dtc(Dtc dtc, common::Span<char> out) noexcept;

/// Adds the DTCs in @p message, the response of one ECU to @p service (0x03, 0x07 or 0x0A), to
/// @p dtcs. Padding (00 00) and codes already in @p dtcs are skipped.
/// @return kOk; kInvalidArgument if @p message is not a response to @p service, or a CAN message
///         has fewer DTCs than it announces; kOutOfRange if @p dtcs is full.
[[nodiscard]] common::Status decode_dtcs(common::Span<const std::uint8_t> message, std::uint8_t service,
                                         DtcList& dtcs) noexcept;

/// The MIL status from service 01 PID 01.
struct MilStatus final {
    bool on{false};               ///< The malfunction indicator lamp ("check engine") is on.
    std::uint8_t dtc_count{0U};  ///< Number of confirmed emission-related DTCs.
};

/// Decodes byte A of the data of service 01 PID 01: bit 7 is the MIL, bits 0 to 6 the number of
/// confirmed DTCs. @return the status; kOutOfRange for empty @p data.
[[nodiscard]] common::Result<MilStatus> decode_mil_status(common::Span<const std::uint8_t> data) noexcept;

}  // namespace elm327

#endif  // ELM327_OBD_DTC_HPP
