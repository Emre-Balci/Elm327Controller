#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common/core/status.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "elm327/obd/pid.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

/// Decodes @p data for @p pid and formats it with the decimals of the PID, or names the error.
std::string decoded(const std::uint8_t pid, const std::vector<std::uint8_t>& data) {
    std::string text{"unknown PID"};
    const common::Result<elm327::PidInfo> info{elm327::find_pid(pid)};
    if (info.has_value()) {
        const common::Result<elm327::Measurement> value{elm327::decode_pid(info.value(), test::bytes_of(data))};
        std::array<char, elm327::Measurement::kMaxFormattedSize> buffer{};
        const common::Result<std::size_t> length{
            value.has_value() ? elm327::format_measurement(value.value(), info.value().decimals, buffer)
                              : common::Result<std::size_t>::failure(value.status())};
        text = length.has_value() ? std::string{buffer.data(), length.value()}
                                  : std::string{common::to_string(length.status())};
    }
    return text;
}

}  // namespace

TEST_CASE(pid_table) {
    const common::Span<const elm327::PidInfo> pids{elm327::known_pids()};
    REQUIRE(!pids.empty());
    for (std::size_t index{0U}; index < pids.size(); ++index) {
        const elm327::PidInfo& info{pids[index]};
        CHECK((index == 0U) || (pids[index - 1U].pid < info.pid));
        CHECK(!info.name.empty());
        CHECK(info.decimals <= elm327::kMaxPidDecimals);
        CHECK(info.divisor > 0);
        CHECK((info.formula == elm327::PidFormula::kEncoded) || ((info.value_bytes >= 1U) && (info.value_bytes <= 4U)));
        CHECK(elm327::find_pid(info.pid).value().pid == info.pid);
    }
    const common::Result<elm327::PidInfo> engine_speed{elm327::find_pid(0x0CU)};
    REQUIRE(engine_speed.has_value());
    CHECK(engine_speed.value().name == "Engine speed");
    CHECK(engine_speed.value().unit == "rpm");
    CHECK(elm327::find_pid(0x65U).status() == common::Status::kNotFound);

    CHECK(elm327::is_support_pid(0x00U));
    CHECK(elm327::is_support_pid(0x20U));
    CHECK(elm327::is_support_pid(0xE0U));
    CHECK(!elm327::is_support_pid(0x01U));
    CHECK(!elm327::is_support_pid(0x21U));
}

TEST_CASE(pid_decode_values) {
    CHECK(decoded(0x0CU, {0x1AU, 0xF8U}) == "1726.00");
    CHECK(decoded(0x0CU, {0x1AU, 0xF9U}) == "1726.25");
    CHECK(decoded(0x0CU, {0xFFU, 0xFFU}) == "16383.75");
    CHECK(decoded(0x0DU, {0x2AU}) == "42");
    CHECK(decoded(0x05U, {0x00U}) == "-40");
    CHECK(decoded(0x05U, {0xFFU}) == "215");
    CHECK(decoded(0x04U, {0xFFU}) == "100.0");
    CHECK(decoded(0x04U, {0x4DU}) == "30.2");
    CHECK(decoded(0x06U, {0x00U}) == "-100.0");
    CHECK(decoded(0x06U, {0x80U}) == "0.0");
    CHECK(decoded(0x06U, {0xFFU}) == "99.2");
    CHECK(decoded(0x0EU, {0x00U}) == "-64.0");
    CHECK(decoded(0x0EU, {0x90U}) == "8.0");
    CHECK(decoded(0x10U, {0x01U, 0xF4U}) == "5.00");
    CHECK(decoded(0x14U, {0x5AU, 0xFFU}) == "0.450");  // Byte B is ignored.
    CHECK(decoded(0x22U, {0xFFU, 0xFFU}) == "5177.265");
    CHECK(decoded(0x42U, {0x36U, 0xB0U}) == "14.000");
    CHECK(decoded(0x44U, {0x80U, 0x00U}) == "1.000");
    CHECK(decoded(0x3CU, {0x00U, 0x00U}) == "-40.0");
    CHECK(decoded(0x5DU, {0x00U, 0x00U}) == "-210.00");
    CHECK(decoded(0x61U, {0x00U}) == "-125");
    CHECK(decoded(0xA6U, {0x00U, 0x01U, 0xE2U, 0x40U}) == "12345.6");
    CHECK(decoded(0xA6U, {0xFFU, 0xFFU, 0xFFU, 0xFFU}) == "429496729.5");
}

TEST_CASE(pid_decode_signed) {
    CHECK(decoded(0x32U, {0xFFU, 0xFCU}) == "-1.00");
    CHECK(decoded(0x32U, {0x7FU, 0xFFU}) == "8191.75");
    CHECK(decoded(0x32U, {0x80U, 0x00U}) == "-8192.00");
    CHECK(decoded(0x54U, {0x80U, 0x00U}) == "-32768");
    CHECK(decoded(0x54U, {0x00U, 0x01U}) == "1");
}

TEST_CASE(pid_decode_rounds_once_half_to_even) {
    CHECK(decoded(0x04U, {0x02U}) == "0.8");  // 0.784...
    CHECK(decoded(0x04U, {0x01U}) == "0.4");  // 0.392...
    // Exact ties of 100 * A / 128 - 100: -93.75 and -81.25.
    CHECK(decoded(0x06U, {0x08U}) == "-93.8");
    CHECK(decoded(0x06U, {0x18U}) == "-81.2");
    // 2 * (256 * A + B) / 65536 = 0.0000305... per step.
    CHECK(decoded(0x44U, {0x00U, 0x10U}) == "0.000");
    CHECK(decoded(0x44U, {0x00U, 0x11U}) == "0.001");
}

TEST_CASE(pid_decode_errors) {
    CHECK(decoded(0x01U, {0x82U, 0x07U, 0xE5U, 0x00U}) == "not supported");  // Bit fields.
    CHECK(decoded(0x0CU, {0x1AU}) == "out of range");                         // Too few bytes.
    CHECK(decoded(0x0CU, {}) == "out of range");
    CHECK(decoded(0x65U, {0x00U}) == "unknown PID");

    elm327::PidInfo info{elm327::find_pid(0x0CU).value()};
    const std::vector<std::uint8_t> data{0x1AU, 0xF8U};
    info.value_bytes = 0U;
    CHECK(elm327::decode_pid(info, test::bytes_of(data)).status() == common::Status::kInvalidArgument);
    info.value_bytes = 5U;
    CHECK(elm327::decode_pid(info, test::bytes_of(data)).status() == common::Status::kInvalidArgument);
    info.value_bytes = 2U;
    info.divisor = 0;
    CHECK(elm327::decode_pid(info, test::bytes_of(data)).status() == common::Status::kInvalidArgument);
    info.divisor = 4;
    info.decimals = 4U;
    CHECK(elm327::decode_pid(info, test::bytes_of(data)).status() == common::Status::kInvalidArgument);
    info.decimals = 2U;
    CHECK(elm327::decode_pid(info, test::bytes_of(data)).has_value());
}

TEST_CASE(pid_format_measurement) {
    std::array<char, elm327::Measurement::kMaxFormattedSize> buffer{};
    const auto formatted = [&buffer](const elm327::Measurement value, const std::uint8_t decimals) {
        const common::Result<std::size_t> length{elm327::format_measurement(value, decimals, buffer)};
        return length.has_value() ? std::string{buffer.data(), length.value()} : std::string{"error"};
    };
    CHECK(formatted(elm327::Measurement::from_units(1'726'250), 2U) == "1726.25");
    CHECK(formatted(elm327::Measurement::from_units(1'726'250), 3U) == "1726.250");
    CHECK(formatted(elm327::Measurement::from_units(-40'000), 0U) == "-40");
    CHECK(formatted(elm327::Measurement::from_units(0), 1U) == "0.0");
    CHECK(formatted(elm327::Measurement::from_units(0), 4U) == "error");

    std::array<char, 4U> small{};
    CHECK(elm327::format_measurement(elm327::Measurement::from_units(1'726'250), 2U, small).status() ==
          common::Status::kOutOfRange);
}

TEST_CASE(pid_supported_bitmap) {
    common::ByteSet supported{};
    // The example of SAE J1979: BE 1F A8 13.
    const std::vector<std::uint8_t> range_00{0xBEU, 0x1FU, 0xA8U, 0x13U};
    REQUIRE(common::is_ok(elm327::decode_supported_pids(0x00U, test::bytes_of(range_00), supported)));
    const std::vector<std::uint8_t> listed{0x01U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x0CU, 0x0DU, 0x0EU,
                                           0x0FU, 0x10U, 0x11U, 0x13U, 0x15U, 0x1CU, 0x1FU, 0x20U};
    for (std::uint32_t value{0U}; value <= 0xFFU; ++value) {
        const std::uint8_t pid{static_cast<std::uint8_t>(value)};
        bool expected{false};
        for (const std::uint8_t entry : listed) {
            expected = expected || (entry == pid);
        }
        CHECK(supported.contains(pid) == expected);
    }

    // The last range ends at FF.
    const std::vector<std::uint8_t> all{0xFFU, 0xFFU, 0xFFU, 0xFFU};
    REQUIRE(common::is_ok(elm327::decode_supported_pids(0xE0U, test::bytes_of(all), supported)));
    CHECK(supported.contains(std::uint8_t{0xE1U}));
    CHECK(supported.contains(std::uint8_t{0xFFU}));
    CHECK(!supported.contains(std::uint8_t{0xE0U}));

    CHECK(elm327::decode_supported_pids(0x10U, test::bytes_of(all), supported) == common::Status::kInvalidArgument);
    const std::vector<std::uint8_t> short_data{0xFFU, 0xFFU, 0xFFU};
    CHECK(elm327::decode_supported_pids(0x20U, test::bytes_of(short_data), supported) ==
          common::Status::kOutOfRange);
}
