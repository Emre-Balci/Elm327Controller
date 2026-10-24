#include <string>

#include "common/core/status.hpp"
#include "elm327/adapter/response.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using elm327::Reply;
using test::span_of;

TEST_CASE(response_classify_messages) {
    CHECK(elm327::classify_line(span_of("OK")) == Reply::kOk);
    CHECK(elm327::classify_line(span_of("?")) == Reply::kUnknownCommand);
    CHECK(elm327::classify_line(span_of("NO DATA")) == Reply::kNoData);
    CHECK(elm327::classify_line(span_of("SEARCHING...")) == Reply::kProgress);
    CHECK(elm327::classify_line(span_of("BUS INIT: ...OK")) == Reply::kProgress);
    CHECK(elm327::classify_line(span_of("BUS INIT: ...ERROR")) == Reply::kBusError);
    CHECK(elm327::classify_line(span_of("UNABLE TO CONNECT")) == Reply::kUnableToConnect);
    CHECK(elm327::classify_line(span_of("CAN ERROR")) == Reply::kBusError);
    CHECK(elm327::classify_line(span_of("BUS ERROR")) == Reply::kBusError);
    CHECK(elm327::classify_line(span_of("BUS BUSY")) == Reply::kBusError);
    CHECK(elm327::classify_line(span_of("FB ERROR")) == Reply::kBusError);
    CHECK(elm327::classify_line(span_of("DATA ERROR")) == Reply::kDataError);
    CHECK(elm327::classify_line(span_of("41 0C 1A F8 <DATA ERROR")) == Reply::kDataError);
    CHECK(elm327::classify_line(span_of("<RX ERROR")) == Reply::kDataError);
    CHECK(elm327::classify_line(span_of("BUFFER FULL")) == Reply::kBufferFull);
    CHECK(elm327::classify_line(span_of("STOPPED")) == Reply::kStopped);
    CHECK(elm327::classify_line(span_of("LV RESET")) == Reply::kAdapterError);
    CHECK(elm327::classify_line(span_of("ERR94")) == Reply::kAdapterError);
    CHECK(elm327::classify_line(span_of("ACT ALERT")) == Reply::kAdapterError);
    CHECK(elm327::classify_line(span_of("LP ALERT")) == Reply::kAdapterError);
}

TEST_CASE(response_classify_content) {
    CHECK(elm327::classify_line(span_of("41 0C 1A F8")) == Reply::kContent);
    CHECK(elm327::classify_line(span_of("410C1AF8")) == Reply::kContent);
    CHECK(elm327::classify_line(span_of("ELM327 v1.5")) == Reply::kContent);
    CHECK(elm327::classify_line(span_of("12.6V")) == Reply::kContent);
    CHECK(elm327::classify_line(span_of("ERR")) == Reply::kContent);
    CHECK(elm327::classify_line(span_of("ERR9X")) == Reply::kContent);
    CHECK(elm327::classify_line(span_of("NO DATA YET")) == Reply::kContent);
    // White space around the text does not matter; nothing at all is empty.
    CHECK(elm327::classify_line(span_of("  OK \t")) == Reply::kOk);
    CHECK(elm327::classify_line(span_of("")) == Reply::kEmpty);
    CHECK(elm327::classify_line(span_of("   ")) == Reply::kEmpty);
}

TEST_CASE(response_reply_kinds) {
    CHECK(!elm327::is_error(Reply::kEmpty));
    CHECK(!elm327::is_error(Reply::kProgress));
    CHECK(!elm327::is_error(Reply::kOk));
    CHECK(!elm327::is_error(Reply::kContent));
    CHECK(elm327::is_error(Reply::kUnknownCommand));
    CHECK(elm327::is_error(Reply::kAdapterError));

    CHECK(elm327::status_of(Reply::kContent) == common::Status::kOk);
    CHECK(elm327::status_of(Reply::kNoData) == common::Status::kNotFound);
    CHECK(elm327::status_of(Reply::kUnableToConnect) == common::Status::kDisconnected);
    CHECK(elm327::status_of(Reply::kUnknownCommand) == common::Status::kNotSupported);
    CHECK(elm327::status_of(Reply::kStopped) == common::Status::kInterrupted);
    CHECK(elm327::status_of(Reply::kBusError) == common::Status::kSystemError);

    CHECK(std::string{elm327::to_string(Reply::kNoData)} == "no data");
    CHECK(std::string{elm327::to_string(Reply::kUnableToConnect)} == "unable to connect");
}

TEST_CASE(response_reply_precedence) {
    elm327::Response response{};
    CHECK(response.empty());
    CHECK(response.reply() == Reply::kEmpty);
    CHECK(response.reply_line().empty());

    // Content outranks progress messages.
    response = test::make_response({"SEARCHING...", "41 00 BE 3F A8 13"});
    CHECK(response.size() == 2U);
    CHECK(response.reply() == Reply::kContent);
    CHECK(test::string_of(response.reply_line()) == "41 00 BE 3F A8 13");

    // The first error outranks everything.
    response = test::make_response({"SEARCHING...", "UNABLE TO CONNECT", "CAN ERROR"});
    CHECK(response.reply() == Reply::kUnableToConnect);
    CHECK(test::string_of(response.reply_line()) == "UNABLE TO CONNECT");
    response = test::make_response({"41 0C 1A F8", "CAN ERROR"});
    CHECK(response.reply() == Reply::kBusError);

    response = test::make_response({"OK"});
    CHECK(response.reply() == Reply::kOk);

    response.clear();
    CHECK(response.empty());
    CHECK(response.reply() == Reply::kEmpty);
    CHECK(!response.truncated());
}

TEST_CASE(response_lines_are_trimmed_and_empty_lines_skipped) {
    elm327::Response response{};
    response.add_line(span_of("  41 0C 1A F8 "), false);
    response.add_line(span_of(""), false);
    response.add_line(span_of(" \t "), false);
    REQUIRE(response.size() == 1U);
    CHECK(test::string_of(response.line(0U)) == "41 0C 1A F8");
    CHECK(!response.truncated());

    // A line the framer had to cut marks the response as truncated.
    response.add_line(span_of("OK"), true);
    CHECK(response.truncated());
}

TEST_CASE(response_truncation) {
    elm327::Response response{};
    const std::string long_line(elm327::Response::kMaxLineLength + 5U, 'A');
    response.add_line(span_of(long_line), false);
    REQUIRE(response.size() == 1U);
    CHECK(response.line(0U).size() == elm327::Response::kMaxLineLength);
    CHECK(response.truncated());

    response.clear();
    for (std::size_t index{0U}; index < elm327::Response::kMaxLines; ++index) {
        response.add_line(span_of("41 00 BE 3F A8 13"), false);
    }
    CHECK(!response.truncated());
    // A dropped error line still decides the reply.
    response.add_line(span_of("BUFFER FULL"), false);
    CHECK(response.size() == elm327::Response::kMaxLines);
    CHECK(response.truncated());
    CHECK(response.reply() == Reply::kBufferFull);
    CHECK(response.reply_line().empty());
}
