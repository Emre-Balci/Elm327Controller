#include "elm327/obd/pid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/numeric/integer.hpp"
#include "common/protocol/byte_order.hpp"

namespace elm327 {
namespace {

// Builders that keep the table below readable.

constexpr PidInfo encoded(const std::uint8_t pid, const std::string_view name) noexcept {
    return PidInfo{pid, 0U, PidFormula::kEncoded, 1, 1, 0, 0U, name, ""};
}

constexpr PidInfo linear(const std::uint8_t pid, const std::uint8_t value_bytes, const std::int32_t multiplier,
                         const std::int32_t divisor, const std::int32_t offset, const std::uint8_t decimals,
                         const std::string_view name, const std::string_view unit) noexcept {
    return PidInfo{pid, value_bytes, PidFormula::kLinear, multiplier, divisor, offset, decimals, name, unit};
}

constexpr PidInfo signed_linear(const std::uint8_t pid, const std::uint8_t value_bytes, const std::int32_t multiplier,
                                const std::int32_t divisor, const std::uint8_t decimals, const std::string_view name,
                                const std::string_view unit) noexcept {
    return PidInfo{pid, value_bytes, PidFormula::kSignedLinear, multiplier, divisor, 0, decimals, name, unit};
}

/// A plain number of 1 or 2 bytes, e.g. km/h or seconds.
constexpr PidInfo number(const std::uint8_t pid, const std::uint8_t value_bytes, const std::string_view name,
                         const std::string_view unit) noexcept {
    return linear(pid, value_bytes, 1, 1, 0, 0U, name, unit);
}

/// 100 * A / 255 %.
constexpr PidInfo percent(const std::uint8_t pid, const std::string_view name) noexcept {
    return linear(pid, 1U, 100, 255, 0, 1U, name, "%");
}

/// A - 40 degC.
constexpr PidInfo temperature(const std::uint8_t pid, const std::string_view name) noexcept {
    return linear(pid, 1U, 1, 1, -40, 0U, name, "degC");
}

/// 100 * A / 128 - 100 %: a correction around 0.
constexpr PidInfo trim(const std::uint8_t pid, const std::string_view name) noexcept {
    return linear(pid, 1U, 100, 128, -100, 1U, name, "%");
}

/// A / 200 V (byte B is a fuel trim, or FF when unused).
constexpr PidInfo oxygen_voltage(const std::uint8_t pid, const std::string_view name) noexcept {
    return linear(pid, 1U, 1, 200, 0, 3U, name, "V");
}

/// 2 * (256 * A + B) / 65536: an air-fuel equivalence ratio (bytes C and D are a voltage or current).
constexpr PidInfo lambda(const std::uint8_t pid, const std::string_view name) noexcept {
    return linear(pid, 2U, 2, 65536, 0, 3U, name, "");
}

/// (256 * A + B) / 10 - 40 degC.
constexpr PidInfo catalyst_temperature(const std::uint8_t pid, const std::string_view name) noexcept {
    return linear(pid, 2U, 1, 10, -40, 1U, name, "degC");
}

/// The service 01 PIDs of SAE J1979 up to 64 with a few later ones, in ascending order.
constexpr std::array<PidInfo, 105U> kPids{{
    encoded(0x00U, "PIDs supported [01-20]"),
    encoded(0x01U, "Monitor status since DTCs cleared"),
    encoded(0x02U, "DTC that caused the freeze frame"),
    encoded(0x03U, "Fuel system status"),
    percent(0x04U, "Calculated engine load"),
    temperature(0x05U, "Engine coolant temperature"),
    trim(0x06U, "Short term fuel trim, bank 1"),
    trim(0x07U, "Long term fuel trim, bank 1"),
    trim(0x08U, "Short term fuel trim, bank 2"),
    trim(0x09U, "Long term fuel trim, bank 2"),
    linear(0x0AU, 1U, 3, 1, 0, 0U, "Fuel pressure", "kPa"),
    number(0x0BU, 1U, "Intake manifold absolute pressure", "kPa"),
    linear(0x0CU, 2U, 1, 4, 0, 2U, "Engine speed", "rpm"),
    number(0x0DU, 1U, "Vehicle speed", "km/h"),
    linear(0x0EU, 1U, 1, 2, -64, 1U, "Timing advance", "deg"),
    temperature(0x0FU, "Intake air temperature"),
    linear(0x10U, 2U, 1, 100, 0, 2U, "Mass air flow rate", "g/s"),
    percent(0x11U, "Throttle position"),
    encoded(0x12U, "Commanded secondary air status"),
    encoded(0x13U, "Oxygen sensors present (2 banks)"),
    oxygen_voltage(0x14U, "Oxygen sensor 1 voltage"),
    oxygen_voltage(0x15U, "Oxygen sensor 2 voltage"),
    oxygen_voltage(0x16U, "Oxygen sensor 3 voltage"),
    oxygen_voltage(0x17U, "Oxygen sensor 4 voltage"),
    oxygen_voltage(0x18U, "Oxygen sensor 5 voltage"),
    oxygen_voltage(0x19U, "Oxygen sensor 6 voltage"),
    oxygen_voltage(0x1AU, "Oxygen sensor 7 voltage"),
    oxygen_voltage(0x1BU, "Oxygen sensor 8 voltage"),
    encoded(0x1CU, "OBD standard"),
    encoded(0x1DU, "Oxygen sensors present (4 banks)"),
    encoded(0x1EU, "Auxiliary input status"),
    number(0x1FU, 2U, "Run time since engine start", "s"),
    encoded(0x20U, "PIDs supported [21-40]"),
    number(0x21U, 2U, "Distance traveled with MIL on", "km"),
    linear(0x22U, 2U, 79, 1000, 0, 3U, "Fuel rail pressure (relative to vacuum)", "kPa"),
    linear(0x23U, 2U, 10, 1, 0, 0U, "Fuel rail gauge pressure", "kPa"),
    lambda(0x24U, "Oxygen sensor 1 lambda, voltage type"),
    lambda(0x25U, "Oxygen sensor 2 lambda, voltage type"),
    lambda(0x26U, "Oxygen sensor 3 lambda, voltage type"),
    lambda(0x27U, "Oxygen sensor 4 lambda, voltage type"),
    lambda(0x28U, "Oxygen sensor 5 lambda, voltage type"),
    lambda(0x29U, "Oxygen sensor 6 lambda, voltage type"),
    lambda(0x2AU, "Oxygen sensor 7 lambda, voltage type"),
    lambda(0x2BU, "Oxygen sensor 8 lambda, voltage type"),
    percent(0x2CU, "Commanded EGR"),
    trim(0x2DU, "EGR error"),
    percent(0x2EU, "Commanded evaporative purge"),
    percent(0x2FU, "Fuel tank level"),
    number(0x30U, 1U, "Warm-ups since codes cleared", ""),
    number(0x31U, 2U, "Distance traveled since codes cleared", "km"),
    signed_linear(0x32U, 2U, 1, 4, 2U, "Evaporative system vapor pressure", "Pa"),
    number(0x33U, 1U, "Absolute barometric pressure", "kPa"),
    lambda(0x34U, "Oxygen sensor 1 lambda, current type"),
    lambda(0x35U, "Oxygen sensor 2 lambda, current type"),
    lambda(0x36U, "Oxygen sensor 3 lambda, current type"),
    lambda(0x37U, "Oxygen sensor 4 lambda, current type"),
    lambda(0x38U, "Oxygen sensor 5 lambda, current type"),
    lambda(0x39U, "Oxygen sensor 6 lambda, current type"),
    lambda(0x3AU, "Oxygen sensor 7 lambda, current type"),
    lambda(0x3BU, "Oxygen sensor 8 lambda, current type"),
    catalyst_temperature(0x3CU, "Catalyst temperature, bank 1 sensor 1"),
    catalyst_temperature(0x3DU, "Catalyst temperature, bank 2 sensor 1"),
    catalyst_temperature(0x3EU, "Catalyst temperature, bank 1 sensor 2"),
    catalyst_temperature(0x3FU, "Catalyst temperature, bank 2 sensor 2"),
    encoded(0x40U, "PIDs supported [41-60]"),
    encoded(0x41U, "Monitor status this drive cycle"),
    linear(0x42U, 2U, 1, 1000, 0, 3U, "Control module voltage", "V"),
    linear(0x43U, 2U, 100, 255, 0, 1U, "Absolute load value", "%"),
    lambda(0x44U, "Commanded air-fuel equivalence ratio"),
    percent(0x45U, "Relative throttle position"),
    temperature(0x46U, "Ambient air temperature"),
    percent(0x47U, "Absolute throttle position B"),
    percent(0x48U, "Absolute throttle position C"),
    percent(0x49U, "Accelerator pedal position D"),
    percent(0x4AU, "Accelerator pedal position E"),
    percent(0x4BU, "Accelerator pedal position F"),
    percent(0x4CU, "Commanded throttle actuator"),
    number(0x4DU, 2U, "Time run with MIL on", "min"),
    number(0x4EU, 2U, "Time since trouble codes cleared", "min"),
    encoded(0x4FU, "Maximum lambda, O2 sensor voltage and current, and MAP"),
    linear(0x50U, 1U, 10, 1, 0, 0U, "Maximum mass air flow rate", "g/s"),
    encoded(0x51U, "Fuel type"),
    percent(0x52U, "Ethanol fuel"),
    linear(0x53U, 2U, 1, 200, 0, 3U, "Absolute evaporative system vapor pressure", "kPa"),
    signed_linear(0x54U, 2U, 1, 1, 0U, "Evaporative system vapor pressure (wide range)", "Pa"),
    trim(0x55U, "Short term secondary O2 sensor trim, bank 1"),
    trim(0x56U, "Long term secondary O2 sensor trim, bank 1"),
    trim(0x57U, "Short term secondary O2 sensor trim, bank 2"),
    trim(0x58U, "Long term secondary O2 sensor trim, bank 2"),
    linear(0x59U, 2U, 10, 1, 0, 0U, "Fuel rail absolute pressure", "kPa"),
    percent(0x5AU, "Relative accelerator pedal position"),
    percent(0x5BU, "Hybrid battery pack remaining life"),
    temperature(0x5CU, "Engine oil temperature"),
    linear(0x5DU, 2U, 1, 128, -210, 2U, "Fuel injection timing", "deg"),
    linear(0x5EU, 2U, 1, 20, 0, 2U, "Engine fuel rate", "L/h"),
    encoded(0x5FU, "Emission requirements"),
    encoded(0x60U, "PIDs supported [61-80]"),
    linear(0x61U, 1U, 1, 1, -125, 0U, "Driver's demand engine torque", "%"),
    linear(0x62U, 1U, 1, 1, -125, 0U, "Actual engine torque", "%"),
    number(0x63U, 2U, "Engine reference torque", "Nm"),
    encoded(0x64U, "Engine percent torque data"),
    encoded(0x80U, "PIDs supported [81-A0]"),
    encoded(0xA0U, "PIDs supported [A1-C0]"),
    linear(0xA6U, 4U, 1, 10, 0, 1U, "Odometer", "km"),
    encoded(0xC0U, "PIDs supported [C1-E0]"),
}};

template <std::size_t Size>
constexpr bool is_ascending(const std::array<PidInfo, Size>& table) noexcept {
    bool ascending{true};
    for (std::size_t index{1U}; index < table.size(); ++index) {
        ascending = ascending && (table[index - 1U].pid < table[index].pid);
    }
    return ascending;
}

// Also catches a wrong array size: missing entries would be PID 00 at the end.
static_assert(is_ascending(kPids), "kPids must be in ascending PID order");

/// 10^0 to 10^kMaxPidDecimals.
constexpr std::array<std::int64_t, std::size_t{kMaxPidDecimals} + 1U> kPowersOfTen{{1, 10, 100, 1000}};

/// Most data bytes a formula reads. raw then stays below 2^32, far from overflowing the arithmetic.
constexpr std::uint8_t kMaxValueBytes{4U};

constexpr std::uint32_t kBitsPerByte{8U};

/// Bytes of a "supported PIDs" response, and the PIDs it covers.
constexpr std::size_t kSupportBytes{4U};
constexpr std::size_t kPidsPerRange{32U};

/// The data bytes the formula reads: unsigned, or two's complement for kSignedLinear.
common::Result<std::int64_t> raw_value(const PidInfo& info, const common::Span<const std::uint8_t> data) noexcept {
    using IntResult = common::Result<std::int64_t>;
    IntResult result{IntResult::failure(common::Status::kInvalidArgument)};
    if ((info.value_bytes > 0U) && (info.value_bytes <= kMaxValueBytes)) {
        const common::Result<std::uint64_t> field{common::read_big_endian_field(data, 0U, info.value_bytes)};
        if (field.has_value()) {
            // Values from 2^(bits - 1) on are negative in two's complement. Both operands are below 2^32.
            const std::uint64_t modulus{std::uint64_t{1U} << (kBitsPerByte * info.value_bytes)};
            const std::int64_t value{static_cast<std::int64_t>(field.value())};
            const bool negative{(info.formula == PidFormula::kSignedLinear) && (field.value() >= (modulus / 2U))};
            result = IntResult::success(negative ? (value - static_cast<std::int64_t>(modulus)) : value);
        } else {
            result = IntResult::failure(field.status());
        }
    }
    return result;
}

/// The value of @p raw in units of 10^-kMaxPidDecimals, rounded to the resolution of the PID.
/// @pre info.decimals <= kMaxPidDecimals, info.divisor > 0
common::Result<std::int64_t> scale(const PidInfo& info, const std::int64_t raw) noexcept {
    using IntResult = common::Result<std::int64_t>;
    // value * 10^decimals = (raw * multiplier + offset * divisor) * 10^decimals / divisor. The
    // quotient of the exact value is rounded half to even, and that is the only rounding.
    const std::int64_t resolution{kPowersOfTen[info.decimals]};
    const std::int64_t divisor{info.divisor};
    IntResult units{common::checked_mul(raw, std::int64_t{info.multiplier})};
    if (units.has_value()) {
        const IntResult offset{common::checked_mul(std::int64_t{info.offset}, divisor)};
        units = offset.has_value() ? common::checked_add(units.value(), offset.value()) : offset;
    }
    if (units.has_value()) {
        units = common::checked_mul(units.value(), resolution);
    }
    if (units.has_value()) {
        const common::Result<common::Decimal<0U>> quotient{
            common::Decimal<0U>::from_units(units.value()).divide(divisor, common::RoundingMode::kHalfEven)};
        units = quotient.has_value() ? IntResult::success(quotient.value().units())
                                     : IntResult::failure(quotient.status());
    }
    if (units.has_value()) {
        // The digits beyond the resolution are zero.
        const std::size_t missing{std::size_t{kMaxPidDecimals} - std::size_t{info.decimals}};
        units = common::checked_mul(units.value(), kPowersOfTen[missing]);
    }
    return units;
}

}  // namespace

common::Result<PidInfo> find_pid(const std::uint8_t pid) noexcept {
    common::Result<PidInfo> result{common::Result<PidInfo>::failure(common::Status::kNotFound)};
    bool found{false};
    for (std::size_t index{0U}; (!found) && (index < kPids.size()); ++index) {
        found = kPids[index].pid == pid;
        if (found) {
            result = common::Result<PidInfo>::success(kPids[index]);
        }
    }
    return result;
}

common::Span<const PidInfo> known_pids() noexcept {
    return common::Span<const PidInfo>{kPids.data(), kPids.size()};
}

common::Result<Measurement> decode_pid(const PidInfo& info, const common::Span<const std::uint8_t> data) noexcept {
    using MeasurementResult = common::Result<Measurement>;
    using IntResult = common::Result<std::int64_t>;
    MeasurementResult result{MeasurementResult::failure(common::Status::kNotSupported)};
    if (info.formula != PidFormula::kEncoded) {
        const bool valid{(info.divisor > 0) && (info.decimals <= kMaxPidDecimals)};
        const IntResult raw{valid ? raw_value(info, data) : IntResult::failure(common::Status::kInvalidArgument)};
        const IntResult units{raw.has_value() ? scale(info, raw.value()) : raw};
        result = units.has_value() ? MeasurementResult::success(Measurement::from_units(units.value()))
                                   : MeasurementResult::failure(units.status());
    }
    return result;
}

common::Result<std::size_t> format_measurement(const Measurement value, const std::uint8_t decimals,
                                               const common::Span<char> out) noexcept {
    using SizeResult = common::Result<std::size_t>;
    SizeResult result{SizeResult::failure(common::Status::kInvalidArgument)};
    std::array<char, Measurement::kMaxFormattedSize> text{};
    const SizeResult formatted{value.format(text)};
    if ((decimals <= kMaxPidDecimals) && formatted.has_value()) {
        // format() writes all kMaxPidDecimals digits after the point. Drop the unwanted ones, and
        // the point as well when no digit remains.
        const std::size_t point{(decimals == 0U) ? 1U : 0U};
        const std::size_t dropped{(std::size_t{kMaxPidDecimals} - std::size_t{decimals}) + point};
        const std::size_t length{formatted.value() - dropped};
        if (length <= out.size()) {
            for (std::size_t index{0U}; index < length; ++index) {
                out[index] = text[index];
            }
            result = SizeResult::success(length);
        } else {
            result = SizeResult::failure(common::Status::kOutOfRange);
        }
    }
    return result;
}

common::Status decode_supported_pids(const std::uint8_t base, const common::Span<const std::uint8_t> data,
                                     common::ByteSet& supported) noexcept {
    common::Status status{is_support_pid(base) ? common::Status::kOk : common::Status::kInvalidArgument};
    if (is_ok(status) && (data.size() < kSupportBytes)) {
        status = common::Status::kOutOfRange;
    }
    for (std::size_t bit{0U}; is_ok(status) && (bit < kPidsPerRange); ++bit) {
        if (common::test_bit_msb_first(data, bit).value_or(false)) {
            // The last range ends at FF: its bit 31 would be PID 100.
            const std::size_t pid{std::size_t{base} + bit + 1U};
            if (pid <= 0xFFU) {
                supported = supported.with(static_cast<std::uint8_t>(pid));
            }
        }
    }
    return status;
}

}  // namespace elm327
