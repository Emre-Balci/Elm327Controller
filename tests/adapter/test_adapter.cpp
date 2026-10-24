#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/adapter/response.hpp"
#include "sim/fake_elm327.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;

using elm327::Reply;

namespace {

/// The traffic a TraceFunction saw.
struct Trace final {
    std::vector<std::string> commands{};
    std::vector<std::string> lines{};
};

void record(const elm327::TraceEvent event, const common::Span<const char> text, void* const context) noexcept {
    Trace* const trace{static_cast<Trace*>(context)};
    if (event == elm327::TraceEvent::kCommand) {
        trace->commands.push_back(test::string_of(text));
    } else {
        trace->lines.push_back(test::string_of(text));
    }
}

bool open(elm327::Adapter& adapter, const elm327::sim::FakeElm327& fake) {
    return fake.valid() && common::is_ok(adapter.open(fake.device_path().c_str(), elm327::AdapterOptions{}));
}

}  // namespace

TEST_CASE(adapter_closed) {
    elm327::Adapter adapter{};
    elm327::Response response{};
    CHECK(!adapter.is_open());
    CHECK(adapter.open("/nonexistent/ttyUSB9", elm327::AdapterOptions{}) == common::Status::kNotFound);
    CHECK(adapter.open("/dev/null", elm327::AdapterOptions{}) == common::Status::kInvalidArgument);
    CHECK(adapter.execute("ATI", response) == common::Status::kInvalidArgument);
    CHECK(adapter.initialize('0') == common::Status::kInvalidArgument);
    CHECK(common::is_ok(adapter.close()));
    CHECK(elm327::is_valid_protocol('0'));
    CHECK(elm327::is_valid_protocol('9'));
    CHECK(elm327::is_valid_protocol('C'));
    CHECK(!elm327::is_valid_protocol('D'));
    CHECK(!elm327::is_valid_protocol('c'));
}

TEST_CASE(adapter_initialize) {
    elm327::sim::FakeElm327 fake{};
    elm327::Adapter adapter{};
    REQUIRE(open(adapter, fake));
    CHECK(adapter.identity().empty());
    CHECK(common::is_ok(adapter.initialize('0')));
    CHECK(test::string_of(adapter.identity()) == "ELM327 v1.5");
    // The adapter is in automatic mode already ("A0"), so ATSP is not sent.
    CHECK(fake.commands() == (std::vector<std::string>{"ATZ", "ATE0", "ATL0", "ATDPN"}));
    CHECK(adapter.initialize('X') == common::Status::kInvalidArgument);
    CHECK(common::is_ok(adapter.close()));
    CHECK(!adapter.is_open());
    CHECK(adapter.identity().empty());
}

TEST_CASE(adapter_selects_protocol_only_when_needed) {
    elm327::sim::FakeElm327 fake{};
    {
        elm327::Adapter adapter{};
        REQUIRE(open(adapter, fake));
        CHECK(common::is_ok(adapter.initialize('6')));
    }
    {
        // The adapter keeps the protocol across resets, so the second time ATSP6 is not needed.
        elm327::Adapter adapter{};
        REQUIRE(open(adapter, fake));
        CHECK(common::is_ok(adapter.initialize('6')));
        CHECK(common::is_ok(adapter.initialize('0')));
    }
    const std::vector<std::string> expected{"ATZ", "ATE0", "ATL0", "ATDPN", "ATSP6",  // Protocol 6 selected.
                                            "ATZ", "ATE0", "ATL0", "ATDPN",           // Still 6.
                                            "ATZ", "ATE0", "ATL0", "ATDPN", "ATSP0"};  // Back to automatic.
    CHECK(fake.commands() == expected);
}

TEST_CASE(adapter_execute_replies) {
    elm327::sim::FakeElm327 fake{};
    elm327::Adapter adapter{};
    REQUIRE(open(adapter, fake));
    REQUIRE(common::is_ok(adapter.initialize('0')));
    elm327::Response response{};

    CHECK(common::is_ok(adapter.execute("ATRV", response)));
    CHECK(response.reply() == Reply::kContent);
    REQUIRE(response.size() == 1U);
    CHECK(test::string_of(response.line(0U)) == "12.6V");

    // An error reply of the adapter is not an error of execute().
    CHECK(common::is_ok(adapter.execute("ATXYZ", response)));
    CHECK(response.reply() == Reply::kUnknownCommand);

    // The first OBD request makes the adapter search for the protocol.
    CHECK(common::is_ok(adapter.execute("0100", response, common::Deadline::after(5s))));
    REQUIRE(response.size() == 2U);
    CHECK(test::string_of(response.line(0U)) == "SEARCHING...");
    CHECK(test::string_of(response.line(1U)) == "41 00 BE 3F 80 13");
    CHECK(common::is_ok(adapter.execute("010A", response)));
    CHECK(response.reply() == Reply::kNoData);

    // With spaces off, the bytes come without separators.
    CHECK(common::is_ok(adapter.execute("ATS0", response)));
    CHECK(common::is_ok(adapter.execute("010C", response)));
    REQUIRE(response.size() == 1U);
    CHECK(test::string_of(response.line(0U)) == "410C1AF8");
}

TEST_CASE(adapter_skips_the_echo) {
    // Before initialize(), echo and line feeds are on.
    elm327::sim::FakeElm327 fake{};
    elm327::Adapter adapter{};
    REQUIRE(open(adapter, fake));
    elm327::Response response{};
    CHECK(common::is_ok(adapter.execute("ATI", response)));
    REQUIRE(response.size() == 1U);
    CHECK(test::string_of(response.line(0U)) == "ELM327 v1.5");

    // The trace sees everything, the echo included.
    Trace trace{};
    adapter.set_trace(&record, &trace);
    CHECK(common::is_ok(adapter.execute("ATRV", response)));
    adapter.set_trace(nullptr, nullptr);
    CHECK(common::is_ok(adapter.execute("ATI", response)));
    CHECK(trace.commands == (std::vector<std::string>{"ATRV"}));
    CHECK(trace.lines == (std::vector<std::string>{"ATRV", "12.6V"}));
}

TEST_CASE(adapter_rejects_invalid_commands) {
    elm327::sim::FakeElm327 fake{};
    elm327::Adapter adapter{};
    REQUIRE(open(adapter, fake));
    elm327::Response response{};
    CHECK(adapter.execute("", response) == common::Status::kInvalidArgument);
    CHECK(adapter.execute("AT\rZ", response) == common::Status::kInvalidArgument);
    const std::string long_command(elm327::Adapter::kMaxCommandLength + 1U, 'A');
    CHECK(adapter.execute(long_command, response) == common::Status::kInvalidArgument);
    const std::string longest_command(elm327::Adapter::kMaxCommandLength, 'A');
    CHECK(common::is_ok(adapter.execute(longest_command, response)));
    CHECK(fake.commands() == (std::vector<std::string>{longest_command}));
}

TEST_CASE(adapter_times_out) {
    elm327::sim::Behaviour behaviour{};
    behaviour.silent = true;
    elm327::sim::FakeElm327 fake{elm327::sim::example_vehicle(), behaviour};
    REQUIRE(fake.valid());
    elm327::AdapterOptions options{};
    options.timeouts.reset = 50ms;
    elm327::Adapter adapter{};
    REQUIRE(common::is_ok(adapter.open(fake.device_path().c_str(), options)));
    elm327::Response response{};

    const common::TimePoint start{common::MonotonicClock::now()};
    CHECK(adapter.execute("ATI", response, common::Deadline::after(50ms)) == common::Status::kTimeout);
    const common::Duration waited{common::MonotonicClock::now() - start};
    CHECK(waited >= 50ms);
    CHECK(waited < 2s);

    // A reset that times out is not repeated.
    CHECK(adapter.initialize('0') == common::Status::kTimeout);
    CHECK((common::MonotonicClock::now() - start) < 2s);
}

TEST_CASE(adapter_cancel) {
    elm327::sim::Behaviour behaviour{};
    behaviour.silent = true;
    elm327::sim::FakeElm327 fake{elm327::sim::example_vehicle(), behaviour};
    elm327::Adapter adapter{};
    REQUIRE(open(adapter, fake));
    test::Event event{};
    REQUIRE(event.descriptor() >= 0);
    adapter.set_cancel_descriptor(event.descriptor());
    elm327::Response response{};

    // Cancelled while waiting for the reply.
    const common::TimePoint start{common::MonotonicClock::now()};
    std::thread canceller{[&event]() {
        std::this_thread::sleep_for(30ms);
        event.signal();
    }};
    CHECK(adapter.execute("ATI", response, common::Deadline::after(5s)) == common::Status::kInterrupted);
    canceller.join();
    CHECK((common::MonotonicClock::now() - start) < 2s);

    // Once the descriptor is readable, every operation stops at once.
    CHECK(adapter.execute("ATI", response, common::Deadline::after(5s)) == common::Status::kInterrupted);
    CHECK(adapter.initialize('0') == common::Status::kInterrupted);
    CHECK((common::MonotonicClock::now() - start) < 2s);
}

TEST_CASE(adapter_disconnected) {
    elm327::sim::FakeElm327 fake{};
    elm327::Adapter adapter{};
    REQUIRE(open(adapter, fake));
    REQUIRE(common::is_ok(adapter.initialize('0')));
    fake.hang_up();
    elm327::Response response{};
    CHECK(adapter.execute("ATI", response) == common::Status::kDisconnected);
}
