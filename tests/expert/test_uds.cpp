#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/expert/uds.hpp"
#include "elm327/obd/message.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using elm327::expert::UdsCommand;

std::string text_of(const UdsCommand& command) {
    const std::string_view view{command.view()};
    return std::string{view.data(), view.size()};
}

common::Span<const std::uint8_t> span_of(const std::vector<std::uint8_t>& bytes) {
    return common::Span<const std::uint8_t>{bytes.data(), bytes.size()};
}

}  // namespace

TEST_CASE(uds_builders_format_requests) {
    namespace e = elm327::expert;
    UdsCommand command{};

    CHECK(e::uds_read_data_by_identifier(0xF190U, command));
    CHECK(text_of(command) == "22F190");

    const std::vector<std::uint8_t> value{0x01U};
    CHECK(e::uds_write_data_by_identifier(0x0407U, span_of(value), command));
    CHECK(text_of(command) == "2E040701");

    CHECK(e::uds_session_control(e::DiagnosticSession::kExtended, command));
    CHECK(text_of(command) == "1003");

    CHECK(e::uds_tester_present(false, command));
    CHECK(text_of(command) == "3E00");  // Asks for the answer.
    CHECK(e::uds_tester_present(true, command));
    CHECK(text_of(command) == "3E80");  // Suppress-positive-response: a silent keep-alive.

    CHECK(e::uds_ecu_reset(e::EcuReset::kHardReset, command));
    CHECK(text_of(command) == "1101");

    const std::vector<std::uint8_t> none{};
    CHECK(e::uds_routine_control(e::RoutineControlType::kStart, 0xFF00U, span_of(none), command));
    CHECK(text_of(command) == "3101FF00");

    const std::vector<std::uint8_t> data{0xAAU};
    CHECK(e::uds_routine_control(e::RoutineControlType::kStart, 0x0203U, span_of(data), command));
    CHECK(text_of(command) == "31010203AA");

    CHECK(e::uds_security_request_seed(0x01U, command));
    CHECK(text_of(command) == "2701");

    const std::vector<std::uint8_t> key{0x11U, 0x22U};
    CHECK(e::uds_security_send_key(0x02U, span_of(key), command));
    CHECK(text_of(command) == "27021122");
}

TEST_CASE(uds_request_rejects_overlong_value) {
    namespace e = elm327::expert;
    UdsCommand command{};
    // A value that overflows the 64-character command leaves the output empty and returns false.
    const std::vector<std::uint8_t> huge(40U, std::uint8_t{0xABU});
    CHECK(!e::uds_write_data_by_identifier(0xF190U, span_of(huge), command));
    CHECK(command.empty());
}

TEST_CASE(uds_parse_positive_reply) {
    namespace e = elm327::expert;
    const std::vector<std::uint8_t> message{0x62U, 0xF1U, 0x90U, 0x57U, 0x30U};
    const common::Result<e::UdsReply> reply{e::parse_uds_reply(span_of(message))};
    REQUIRE(reply.has_value());
    CHECK(reply.value().positive);
    CHECK(reply.value().service == 0x22U);  // The request service the reply echoes.
    CHECK(!reply.value().pending);
    CHECK(test::vector_of(reply.value().data) == std::vector<std::uint8_t>({0xF1U, 0x90U, 0x57U, 0x30U}));
}

TEST_CASE(uds_parse_negative_reply) {
    namespace e = elm327::expert;
    const std::vector<std::uint8_t> message{0x7FU, 0x22U, 0x31U};
    const common::Result<e::UdsReply> reply{e::parse_uds_reply(span_of(message))};
    REQUIRE(reply.has_value());
    CHECK(!reply.value().positive);
    CHECK(reply.value().service == 0x22U);
    CHECK(reply.value().nrc == 0x31U);
    CHECK(!reply.value().pending);

    const std::vector<std::uint8_t> pending{0x7FU, 0x22U, 0x78U};
    const common::Result<e::UdsReply> wait{e::parse_uds_reply(span_of(pending))};
    REQUIRE(wait.has_value());
    CHECK(wait.value().pending);
}

TEST_CASE(uds_parse_rejects_malformed) {
    namespace e = elm327::expert;
    const std::vector<std::uint8_t> empty{};
    CHECK(!e::parse_uds_reply(span_of(empty)).has_value());
    const std::vector<std::uint8_t> short_negative{0x7FU, 0x22U};
    CHECK(!e::parse_uds_reply(span_of(short_negative)).has_value());
}

TEST_CASE(uds_names) {
    namespace e = elm327::expert;
    CHECK(std::string_view{e::uds_service_name(0x22U)} == "ReadDataByIdentifier");
    CHECK(std::string_view{e::uds_service_name(0x2EU)} == "WriteDataByIdentifier");
    CHECK(std::string_view{e::uds_service_name(0x31U)} == "RoutineControl");
    CHECK(std::string_view{e::uds_service_name(0xAAU)} == "unknown service");

    CHECK(std::string_view{e::nrc_name(0x11U)} == "serviceNotSupported");
    CHECK(std::string_view{e::nrc_name(0x33U)} == "securityAccessDenied");
    CHECK(std::string_view{e::nrc_name(0x35U)} == "invalidKey");
    CHECK(std::string_view{e::nrc_name(0x78U)} == "requestCorrectlyReceived-ResponsePending");
    CHECK(std::string_view{e::nrc_name(0x99U)} == "manufacturer-specific / reserved");
}

TEST_CASE(uds_decode_response) {
    namespace e = elm327::expert;
    elm327::MessageList scratch{};
    e::UdsExchange exchange{};

    // A single frame.
    const elm327::Response single{test::make_response({"62 04 07 00"})};
    REQUIRE(common::is_ok(e::decode_uds_response(single, scratch, exchange)));
    CHECK(exchange.positive);
    CHECK(exchange.service == 0x22U);
    CHECK(test::vector_of(common::Span<const std::uint8_t>{exchange.data.data(), exchange.data.size()}) ==
          std::vector<std::uint8_t>({0x04U, 0x07U, 0x00U}));
    CHECK(!exchange.truncated);

    // Several CAN frames make one message: the VIN of DID F190, which the exchange owns.
    const elm327::Response multi{test::make_response(
        {"SEARCHING...", "014", "0: 62 F1 90 31 44 34", "1: 47 50 30 30 52 35 35", "2: 42 31 32 33 34 35 36"})};
    REQUIRE(common::is_ok(e::decode_uds_response(multi, scratch, exchange)));
    CHECK(exchange.positive);
    REQUIRE(exchange.data.size() == 19U);  // F1 90 and the 17 characters.
    CHECK(exchange.data[2U] == 0x31U);
    scratch.clear();  // The exchange stays valid when the scratch is reused.
    CHECK(exchange.data[18U] == 0x36U);

    // A negative reply.
    const elm327::Response negative{test::make_response({"7F 2E 33"})};
    REQUIRE(common::is_ok(e::decode_uds_response(negative, scratch, exchange)));
    CHECK(!exchange.positive);
    CHECK(exchange.service == 0x2EU);
    CHECK(exchange.nrc == 0x33U);
    CHECK(exchange.data.empty());

    // Nothing to decode, and lines that are not hexadecimal bytes (headers on).
    CHECK(e::decode_uds_response(test::make_response({"SEARCHING..."}), scratch, exchange) ==
          common::Status::kNotFound);
    CHECK(e::decode_uds_response(test::make_response({"7E8 03 62 04 07"}), scratch, exchange) ==
          common::Status::kInvalidArgument);
}

TEST_CASE(uds_format_printable) {
    namespace e = elm327::expert;
    std::array<char, 8U> out{};
    const std::vector<std::uint8_t> vin_start{0x31U, 0x44U, 0x34U};
    const common::Result<std::size_t> text{e::format_printable(span_of(vin_start), out)};
    REQUIRE(text.has_value());
    CHECK(std::string(out.data(), text.value()) == "1D4");

    const std::vector<std::uint8_t> edges{0x20U, 0x7EU};  // The first and the last printable byte.
    REQUIRE(e::format_printable(span_of(edges), out).has_value());
    CHECK(std::string(out.data(), 2U) == " ~");

    const std::vector<std::uint8_t> binary{0x41U, 0x00U};
    CHECK(e::format_printable(span_of(binary), out).status() == common::Status::kInvalidArgument);
    const std::vector<std::uint8_t> high{0x7FU};
    CHECK(e::format_printable(span_of(high), out).status() == common::Status::kInvalidArgument);
    const std::vector<std::uint8_t> none{};
    CHECK(e::format_printable(span_of(none), out).status() == common::Status::kInvalidArgument);
    const std::vector<std::uint8_t> long_text(9U, std::uint8_t{0x41U});
    CHECK(e::format_printable(span_of(long_text), out).status() == common::Status::kOutOfRange);
}
