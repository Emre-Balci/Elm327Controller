#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/containers/span.hpp"
#include "elm327/expert/uds.hpp"
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

    CHECK(e::uds_tester_present(command));
    CHECK(text_of(command) == "3E80");

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
