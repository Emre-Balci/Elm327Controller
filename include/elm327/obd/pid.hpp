/// @file pid.hpp
/// @brief Service 01 parameters (PIDs) of SAE J1979: what they are and how to decode their data.
///
/// A service 01 request "01 <PID>" asks the vehicle for the current value of one parameter, e.g.
/// "010C" for the engine speed. The answer "41 0C 1A F8" carries the data bytes A = 1A and B = F8,
/// and the formula of the PID turns them into a physical value: (256 * A + B) / 4 = 1726 rpm.
///
/// decode_pid() evaluates the formula exactly in integer arithmetic and rounds the result once, to
/// the resolution of the PID (PidInfo::decimals), so a value always prints the same way. PIDs whose
/// data are bit fields or enumerations, e.g. PID 01 (monitor status), have no single value; their
/// data bytes are shown as they are.

#ifndef ELM327_OBD_PID_HPP
#define ELM327_OBD_PID_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/numeric/decimal.hpp"
#include "common/protocol/delimited_framer.hpp"

namespace elm327 {

/// How the data bytes of a PID become its value.
enum class PidFormula : std::uint8_t {
    /// value = raw * multiplier / divisor + offset, where raw is the first value_bytes data bytes
    /// as an unsigned big-endian number.
    kLinear,
    /// Like kLinear, with raw as a two's complement number.
    kSignedLinear,
    /// Bit fields or enumerations, without a single value.
    kEncoded,
};

/// Description of a service 01 PID.
struct PidInfo final {
    std::uint8_t pid{0U};
    /// Number of data bytes the formula reads (1 to 4): A, AB, ABC or ABCD. Some PIDs send more,
    /// e.g. a second value in byte B, which the formula ignores.
    std::uint8_t value_bytes{0U};
    PidFormula formula{PidFormula::kEncoded};
    std::int32_t multiplier{1};
    std::int32_t divisor{1};  ///< Positive.
    std::int32_t offset{0};
    /// Digits after the decimal point: the resolution the value is rounded to, at most
    /// kMaxPidDecimals.
    std::uint8_t decimals{0U};
    std::string_view name{};
    /// Unit in ASCII, e.g. "rpm", "km/h" or "degC"; empty for counts, ratios and kEncoded.
    std::string_view unit{};
};

/// Most decimals of a PID value.
inline constexpr std::uint8_t kMaxPidDecimals{3U};

/// A decoded PID value, exact to kMaxPidDecimals digits after the point. decode_pid() rounds it to
/// the resolution of its PID, so the digits beyond PidInfo::decimals are zero.
using Measurement = common::Decimal<kMaxPidDecimals>;

/// @return true for the PIDs 00, 20, 40, ..., E0, whose data list which of the next 32 PIDs the
///         vehicle supports.
[[nodiscard]] constexpr bool is_support_pid(const std::uint8_t pid) noexcept {
    return (static_cast<std::uint32_t>(pid) % 0x20U) == 0U;
}

/// @return the description of @p pid; kNotFound for a PID that known_pids() does not list.
[[nodiscard]] common::Result<PidInfo> find_pid(std::uint8_t pid) noexcept;

/// Every PID this library can name, in ascending order.
[[nodiscard]] common::Span<const PidInfo> known_pids() noexcept;

/// Decodes @p data, the data bytes of a response to PID info.pid (after "41 <pid>").
/// @return the value; kNotSupported for a kEncoded PID; kOutOfRange if @p data has fewer than
///         info.value_bytes bytes; kInvalidArgument for an invalid @p info (value_bytes outside 1
///         to 4, a divisor below 1, or too many decimals); kOverflow if the value does not fit.
[[nodiscard]] common::Result<Measurement> decode_pid(const PidInfo& info,
                                                     common::Span<const std::uint8_t> data) noexcept;

/// Writes @p value with @p decimals digits after the point to the start of @p out, e.g. "1726.25"
/// or "-40". No NUL is written. The digits that are cut off must be zeros, as decode_pid() makes
/// them for the decimals of the PID.
/// @return the number of characters written; kInvalidArgument if @p decimals exceeds
///         kMaxPidDecimals; kOutOfRange if @p out is too small (Measurement::kMaxFormattedSize
///         always fits).
[[nodiscard]] common::Result<std::size_t> format_measurement(Measurement value, std::uint8_t decimals,
                                                             common::Span<char> out) noexcept;

/// Adds the PIDs that @p data, the 4 data bytes of a response to the support PID @p base (00, 20,
/// ..., E0), lists to @p supported: bit 7 of A is PID base + 1, bit 0 of D is PID base + 0x20.
/// @return kOk; kInvalidArgument if @p base is not a support PID; kOutOfRange if @p data has fewer
///         than 4 bytes.
[[nodiscard]] common::Status decode_supported_pids(std::uint8_t base, common::Span<const std::uint8_t> data,
                                                   common::ByteSet& supported) noexcept;

}  // namespace elm327

#endif  // ELM327_OBD_PID_HPP
