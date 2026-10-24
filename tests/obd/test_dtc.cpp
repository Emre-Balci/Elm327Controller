#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common/core/status.hpp"
#include "elm327/obd/dtc.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using Bytes = std::vector<std::uint8_t>;

namespace {

std::string formatted(const elm327::Dtc dtc) {
    std::array<char, elm327::kDtcTextSize> text{};
    const common::Result<std::size_t> length{elm327::format_dtc(dtc, text)};
    return length.has_value() ? std::string{text.data(), length.value()} : std::string{"error"};
}

std::vector<elm327::Dtc> codes_of(const elm327::DtcList& dtcs) {
    return std::vector<elm327::Dtc>{dtcs.begin(), dtcs.end()};
}

}  // namespace

TEST_CASE(dtc_format) {
    CHECK(formatted(0x0133U) == "P0133");
    CHECK(formatted(0x0420U) == "P0420");
    CHECK(formatted(0x4123U) == "C0123");
    CHECK(formatted(0x8123U) == "B0123");
    CHECK(formatted(0xC123U) == "U0123");
    CHECK(formatted(0x3FFFU) == "P3FFF");
    CHECK(formatted(0xFFFFU) == "U3FFF");
    std::array<char, 4U> small{};
    CHECK(elm327::format_dtc(0x0133U, small).status() == common::Status::kOutOfRange);

    CHECK(elm327::request_service(elm327::DtcKind::kStored) == 0x03U);
    CHECK(elm327::request_service(elm327::DtcKind::kPending) == 0x07U);
    CHECK(elm327::request_service(elm327::DtcKind::kPermanent) == 0x0AU);
    CHECK(std::string{elm327::to_string(elm327::DtcKind::kPermanent)} == "permanent");
}

TEST_CASE(dtc_decode_can) {
    elm327::DtcList dtcs{};
    // The number of DTCs, then the DTCs.
    const Bytes ecm{0x43U, 0x02U, 0x01U, 0x33U, 0x04U, 0x20U};
    REQUIRE(common::is_ok(elm327::decode_dtcs(test::bytes_of(ecm), 0x03U, dtcs)));
    CHECK(codes_of(dtcs) == (std::vector<elm327::Dtc>{0x0133U, 0x0420U}));
    // A second ECU adds only the codes that are new.
    const Bytes tcm{0x43U, 0x02U, 0x04U, 0x20U, 0x07U, 0x00U};
    REQUIRE(common::is_ok(elm327::decode_dtcs(test::bytes_of(tcm), 0x03U, dtcs)));
    CHECK(codes_of(dtcs) == (std::vector<elm327::Dtc>{0x0133U, 0x0420U, 0x0700U}));

    elm327::DtcList none{};
    const Bytes empty{0x47U, 0x00U};
    REQUIRE(common::is_ok(elm327::decode_dtcs(test::bytes_of(empty), 0x07U, none)));
    CHECK(none.empty());

    // Fewer DTCs than announced.
    const Bytes short_message{0x43U, 0x03U, 0x01U, 0x33U};
    CHECK(elm327::decode_dtcs(test::bytes_of(short_message), 0x03U, none) == common::Status::kInvalidArgument);
}

TEST_CASE(dtc_decode_legacy) {
    elm327::DtcList dtcs{};
    // Three DTCs per message, padded with 00 00.
    const Bytes first{0x43U, 0x01U, 0x33U, 0x04U, 0x20U, 0x01U, 0x71U};
    const Bytes second{0x43U, 0x01U, 0x72U, 0x00U, 0x00U, 0x00U, 0x00U};
    REQUIRE(common::is_ok(elm327::decode_dtcs(test::bytes_of(first), 0x03U, dtcs)));
    REQUIRE(common::is_ok(elm327::decode_dtcs(test::bytes_of(second), 0x03U, dtcs)));
    CHECK(codes_of(dtcs) == (std::vector<elm327::Dtc>{0x0133U, 0x0420U, 0x0171U, 0x0172U}));

    elm327::DtcList none{};
    const Bytes empty{0x4AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    REQUIRE(common::is_ok(elm327::decode_dtcs(test::bytes_of(empty), 0x0AU, none)));
    CHECK(none.empty());
}

TEST_CASE(dtc_decode_errors) {
    elm327::DtcList dtcs{};
    const Bytes pending{0x47U, 0x01U, 0x01U, 0x33U};
    CHECK(elm327::decode_dtcs(test::bytes_of(pending), 0x03U, dtcs) == common::Status::kInvalidArgument);
    CHECK(elm327::decode_dtcs(common::Span<const std::uint8_t>{}, 0x03U, dtcs) == common::Status::kInvalidArgument);

    for (std::size_t index{0U}; index < elm327::kMaxDtcs; ++index) {
        REQUIRE(dtcs.push_back(static_cast<elm327::Dtc>(0x0100U + index)));
    }
    const Bytes one_more{0x43U, 0x01U, 0x03U, 0x00U};
    CHECK(elm327::decode_dtcs(test::bytes_of(one_more), 0x03U, dtcs) == common::Status::kOutOfRange);
}

TEST_CASE(dtc_mil_status) {
    const Bytes lit{0x82U, 0x07U, 0xE5U, 0x00U};
    const common::Result<elm327::MilStatus> on{elm327::decode_mil_status(test::bytes_of(lit))};
    REQUIRE(on.has_value());
    CHECK(on.value().on);
    CHECK(on.value().dtc_count == 2U);

    const Bytes dark{0x00U, 0x07U, 0xE5U, 0x00U};
    const common::Result<elm327::MilStatus> off{elm327::decode_mil_status(test::bytes_of(dark))};
    REQUIRE(off.has_value());
    CHECK(!off.value().on);
    CHECK(off.value().dtc_count == 0U);

    CHECK(elm327::decode_mil_status(common::Span<const std::uint8_t>{}).status() == common::Status::kOutOfRange);
}
