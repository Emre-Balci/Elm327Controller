#include <initializer_list>
#include <string>
#include <string_view>

#include "common/core/status.hpp"
#include "elm327/obd/message.hpp"
#include "elm327/obd/vin.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

/// Decodes the VIN from the reply @p lines, or names the error.
std::string vin_of(const std::initializer_list<std::string_view> lines) {
    elm327::MessageList messages{};
    elm327::Vin vin{};
    common::Status status{elm327::parse_messages(test::make_response(lines), messages)};
    if (common::is_ok(status)) {
        status = elm327::decode_vin(messages, vin);
    }
    return common::is_ok(status) ? test::string_of(vin.text()) : std::string{common::to_string(status)};
}

}  // namespace

TEST_CASE(vin_can) {
    CHECK(vin_of({"014", "0: 49 02 01 31 44 34", "1: 47 50 30 30 52 35 35", "2: 42 31 32 33 34 35 36"}) ==
          "1D4GP00R55B123456");
    // Some ECUs pad the VIN with leading 00 bytes.
    CHECK(vin_of({"017", "0: 49 02 01 00 00 00", "1: 31 44 34 47 50 30 30", "2: 52 35 35 42 31 32 33",
                  "3: 34 35 36 00 00 00 00"}) == "1D4GP00R55B123456");
}

TEST_CASE(vin_legacy) {
    // Five numbered messages, in any order.
    CHECK(vin_of({"49 02 02 44 34 47 50", "49 02 01 00 00 00 31", "49 02 03 30 30 52 35", "49 02 05 33 34 35 36",
                  "49 02 04 35 42 31 32"}) == "1D4GP00R55B123456");
}

TEST_CASE(vin_errors) {
    CHECK(vin_of({"7F 09 12"}) == "not supported");
    CHECK(vin_of({"41 0C 1A F8"}) == "invalid argument");
    CHECK(vin_of({"49 02 01 31 44 34"}) == "invalid argument");
    // A character that is not a digit or a capital letter.
    CHECK(vin_of({"014", "0: 49 02 01 31 44 34", "1: 47 50 30 30 52 35 35", "2: 42 31 32 33 34 35 66"}) ==
          "invalid argument");
    // A missing message: too few characters.
    CHECK(vin_of({"49 02 01 00 00 00 31", "49 02 02 44 34 47 50", "49 02 03 30 30 52 35", "49 02 04 35 42 31 32"}) ==
          "invalid argument");
}
