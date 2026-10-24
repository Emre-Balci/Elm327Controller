#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "common/core/status.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/obd/message.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using Bytes = std::vector<std::uint8_t>;

namespace {

/// The VIN 1D4GP00R55B123456 as the adapter prints a CAN multi-frame reply to 09 02.
const std::initializer_list<std::string_view> kVinFrames{"014", "0: 49 02 01 31 44 34", "1: 47 50 30 30 52 35 35",
                                                         "2: 42 31 32 33 34 35 36"};

common::Status parse(const elm327::Response& response, elm327::MessageList& messages) {
    return elm327::parse_messages(response, messages);
}

}  // namespace

TEST_CASE(message_single_lines) {
    elm327::MessageList messages{};
    // One line per ECU; progress messages carry no data.
    elm327::Response response{test::make_response({"SEARCHING...", "41 00 BE 3F A8 13", "41 00 98 18 80 11"})};
    REQUIRE(common::is_ok(parse(response, messages)));
    REQUIRE(messages.size() == 2U);
    CHECK(test::vector_of(messages[0U].bytes()) == (Bytes{0x41U, 0x00U, 0xBEU, 0x3FU, 0xA8U, 0x13U}));
    CHECK(test::vector_of(messages[1U].bytes()) == (Bytes{0x41U, 0x00U, 0x98U, 0x18U, 0x80U, 0x11U}));

    // Without spaces (ATS0).
    response = test::make_response({"410C1AF8"});
    REQUIRE(common::is_ok(parse(response, messages)));
    REQUIRE(messages.size() == 1U);
    CHECK(test::vector_of(messages[0U].bytes()) == (Bytes{0x41U, 0x0CU, 0x1AU, 0xF8U}));

    // A reply without data lines gives no messages, and earlier messages are cleared.
    response = test::make_response({"SEARCHING..."});
    REQUIRE(common::is_ok(parse(response, messages)));
    CHECK(messages.empty());
}

TEST_CASE(message_multi_frame) {
    elm327::MessageList messages{};
    elm327::Response response{test::make_response(kVinFrames)};
    REQUIRE(common::is_ok(parse(response, messages)));
    REQUIRE(messages.size() == 1U);
    const Bytes vin_message{0x49U, 0x02U, 0x01U, 0x31U, 0x44U, 0x34U, 0x47U, 0x50U, 0x30U, 0x30U,
                            0x52U, 0x35U, 0x35U, 0x42U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U};
    CHECK(test::vector_of(messages[0U].bytes()) == vin_message);

    // The padding of the last frame is dropped, with or without spaces.
    const Bytes dtc_message{0x43U, 0x04U, 0x01U, 0x33U, 0x04U, 0x20U, 0x01U, 0x71U, 0x01U, 0x72U};
    response = test::make_response({"00A", "0: 43 04 01 33 04 20", "1: 01 71 01 72 00 00 00"});
    REQUIRE(common::is_ok(parse(response, messages)));
    REQUIRE(messages.size() == 1U);
    CHECK(test::vector_of(messages[0U].bytes()) == dtc_message);
    response = test::make_response({"00A", "0:430401330420", "1:01710172000000"});
    REQUIRE(common::is_ok(parse(response, messages)));
    REQUIRE(messages.size() == 1U);
    CHECK(test::vector_of(messages[0U].bytes()) == dtc_message);

    // A single-line message after a complete multi-frame one.
    response = test::make_response({"008", "0: 41 00 BE 3F A8 13", "1: 00 00 00 00 00 00 00", "41 00 98 18 80 11"});
    REQUIRE(common::is_ok(parse(response, messages)));
    REQUIRE(messages.size() == 2U);
    CHECK(messages[0U].size() == 8U);
    CHECK(messages[1U].size() == 6U);
}

TEST_CASE(message_frame_numbers_wrap) {
    // 6 + 16 * 7 = 118 bytes: frames 0 to F, then 0 again.
    std::vector<std::string> lines{"076", "0: 00 01 02 03 04 05"};
    std::uint32_t value{6U};
    for (std::uint32_t frame{1U}; frame <= 16U; ++frame) {
        const std::string_view digits{"0123456789ABCDEF"};
        std::string line{std::string(1U, digits[frame % 16U]) + ":"};
        for (std::uint32_t index{0U}; index < 7U; ++index) {
            line += std::string{" "} + digits[(value >> 4U) & 0xFU] + digits[value & 0xFU];
            ++value;
        }
        lines.push_back(line);
    }
    elm327::Response response{};
    for (const std::string& line : lines) {
        response.add_line(test::span_of(line), false);
    }
    elm327::MessageList messages{};
    REQUIRE(common::is_ok(parse(response, messages)));
    REQUIRE(messages.size() == 1U);
    REQUIRE(messages[0U].size() == 118U);
    for (std::size_t index{0U}; index < 118U; ++index) {
        CHECK(messages[0U].bytes()[index] == static_cast<std::uint8_t>(index));
    }
}

TEST_CASE(message_errors) {
    elm327::MessageList messages{};
    const auto status_for = [&messages](const std::initializer_list<std::string_view> lines) {
        return parse(test::make_response(lines), messages);
    };
    // Frames out of order, missing, without a length line, or interrupted by a single line.
    CHECK(status_for({"014", "0: 49 02 01 31 44 34", "2: 42 31 32 33 34 35 36"}) == common::Status::kInvalidArgument);
    CHECK(status_for({"014", "0: 49 02 01 31 44 34", "1: 47 50 30 30 52 35 35"}) == common::Status::kInvalidArgument);
    CHECK(status_for({"0: 49 02 01 31 44 34"}) == common::Status::kInvalidArgument);
    CHECK(status_for({"014", "0: 49 02 01 31 44 34", "41 0C 1A F8"}) == common::Status::kInvalidArgument);
    CHECK(status_for({"014", "014"}) == common::Status::kInvalidArgument);
    CHECK(status_for({"000"}) == common::Status::kInvalidArgument);
    // Not hexadecimal bytes.
    CHECK(status_for({"ELM327 v1.5"}) == common::Status::kInvalidArgument);
    CHECK(status_for({"41 0C 1"}) == common::Status::kInvalidArgument);
    // Too long, too many.
    CHECK(status_for({"101"}) == common::Status::kOutOfRange);
    elm327::Response response{};
    for (std::size_t index{0U}; index <= elm327::kMaxMessages; ++index) {
        response.add_line(test::span_of("41 0C 1A F8"), false);
    }
    CHECK(parse(response, messages) == common::Status::kOutOfRange);
}

TEST_CASE(message_find_response) {
    CHECK(elm327::response_service(0x01U) == 0x41U);
    CHECK(elm327::response_service(0x09U) == 0x49U);

    elm327::MessageList messages{};
    REQUIRE(common::is_ok(parse(test::make_response({"7F 01 12", "41 0C 1A F8"}), messages)));
    const Bytes engine_speed{0x01U, 0x0CU};
    const Bytes vehicle_speed{0x01U, 0x0DU};
    CHECK(elm327::rejects(messages[0U], 0x01U));
    CHECK(!elm327::rejects(messages[0U], 0x09U));
    CHECK(elm327::answers(messages[1U], test::bytes_of(engine_speed)));
    CHECK(!elm327::answers(messages[1U], test::bytes_of(vehicle_speed)));
    CHECK(!elm327::answers(messages[1U], common::Span<const std::uint8_t>{}));

    const auto found = elm327::find_response(messages, test::bytes_of(engine_speed));
    REQUIRE(found.has_value());
    CHECK(test::vector_of(found.value()) == (Bytes{0x41U, 0x0CU, 0x1AU, 0xF8U}));
    // Rejected by one ECU, answered by none.
    CHECK(elm327::find_response(messages, test::bytes_of(vehicle_speed)).status() == common::Status::kNotSupported);

    REQUIRE(common::is_ok(parse(test::make_response({"41 0C 1A F8"}), messages)));
    CHECK(elm327::find_response(messages, test::bytes_of(vehicle_speed)).status() == common::Status::kInvalidArgument);
}
