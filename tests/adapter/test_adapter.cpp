#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/adapter/response.hpp"
#include "sim/demo.hpp"
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
    CHECK(adapter.open(test::kMissingDevice, elm327::AdapterOptions{}) == common::Status::kNotFound);
    CHECK(adapter.open(test::kNotSerialDevice, elm327::AdapterOptions{}) == common::Status::kInvalidArgument);
#if defined(_WIN32)
    // A named pipe, as elm327sim offers one there, that does not exist.
    CHECK(adapter.open("\\\\.\\pipe\\elm327-missing", elm327::AdapterOptions{}) == common::Status::kNotFound);
#endif
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
    REQUIRE(event.valid());
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

TEST_CASE(adapter_settles_after_an_interruption) {
    // The protocol search of the first OBD request takes long; the user interrupts it, and the
    // adapter's late reply must not be taken for the reply to the next command.
    for (const std::chrono::milliseconds search : {150ms, 900ms}) {
        elm327::sim::Behaviour behaviour{};
        behaviour.search_delay = search;
        elm327::sim::FakeElm327 fake{elm327::sim::example_vehicle(), behaviour};
        elm327::Adapter adapter{};
        REQUIRE(open(adapter, fake));
        REQUIRE(common::is_ok(adapter.initialize('0')));
        test::Event event{};
        REQUIRE(event.valid());
        adapter.set_cancel_descriptor(event.descriptor());
        elm327::Response response{};
        std::thread canceller{[&event]() {
            std::this_thread::sleep_for(50ms);
            event.signal();
        }};
        CHECK(adapter.execute("0100", response, common::Deadline::after(5s)) == common::Status::kInterrupted);
        canceller.join();
        event.clear();

        // A short search ends while the adapter waits for it; a long one is stopped. Either way
        // the next command gets its own reply.
        REQUIRE(common::is_ok(adapter.execute("ATRV", response, common::Deadline::after(5s))));
        REQUIRE(response.size() == 1U);
        CHECK(test::string_of(response.line(0U)) == "12.6V");
        REQUIRE(common::is_ok(adapter.execute("010D", response, common::Deadline::after(5s))));
        CHECK(test::string_of(response.line(0U)) == "41 0D 2A");
        const std::vector<std::string> commands{fake.commands()};
        CHECK(commands.back() == "010D");
    }
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

#if !defined(_WIN32)
TEST_CASE(adapter_over_a_stream) {
    elm327::sim::FakeElm327 fake{elm327::sim::example_vehicle(), elm327::sim::Behaviour{},
                                 elm327::sim::Transport::kSocket};
    REQUIRE(fake.valid());
    elm327::Adapter adapter{};
    CHECK(adapter.open_stream(common::kInvalidNativeHandle, elm327::AdapterOptions{}) ==
          common::Status::kInvalidArgument);
    REQUIRE(common::is_ok(adapter.open_stream(fake.take_stream(), elm327::AdapterOptions{})));
    CHECK(adapter.is_open());
    CHECK(fake.take_stream() == -1);
    CHECK(common::is_ok(adapter.initialize('0')));
    CHECK(test::string_of(adapter.identity()) == "ELM327 v1.5");
    elm327::Response response{};
    REQUIRE(common::is_ok(adapter.execute("010D", response, common::Deadline::after(5s))));
    REQUIRE(response.size() == 2U);  // The first OBD request: "SEARCHING..." first.
    CHECK(test::string_of(response.line(1U)) == "41 0D 2A");

    // Only one device at a time: a second stream is refused (and closed), as is a serial device.
    elm327::sim::FakeElm327 other{elm327::sim::example_vehicle(), elm327::sim::Behaviour{},
                                  elm327::sim::Transport::kSocket};
    REQUIRE(other.valid());
    CHECK(adapter.open_stream(other.take_stream(), elm327::AdapterOptions{}) == common::Status::kInvalidArgument);
    elm327::sim::FakeElm327 device{};
    CHECK(!open(adapter, device));
    REQUIRE(common::is_ok(adapter.execute("ATRV", response, common::Deadline::after(5s))));
    CHECK(test::string_of(response.line(0U)) == "12.6V");

    // The other end goes away, like an unplugged adapter: no SIGPIPE, but kDisconnected.
    fake.hang_up();
    CHECK(adapter.execute("ATI", response) == common::Status::kDisconnected);
    CHECK(common::is_ok(adapter.close()));
    CHECK(!adapter.is_open());
}

TEST_CASE(adapter_and_the_demo_simulator) {
    elm327::Adapter adapter{};
    REQUIRE(common::is_ok(adapter.open_stream(elm327::sim::start_demo(), elm327::AdapterOptions{})));
    REQUIRE(common::is_ok(adapter.initialize('0')));
    // Nobody takes its values for those of a car.
    CHECK(test::string_of(adapter.identity()) == "ELM327 v1.5 (simulator)");
    elm327::Response response{};
    REQUIRE(common::is_ok(adapter.execute("010C", response, common::Deadline::after(5s))));
    CHECK(test::string_of(response.line(response.size() - 1U)).rfind("41 0C ", 0U) == 0U);

    // The next demo replaces it: the link to the old one ends.
    elm327::Adapter next{};
    REQUIRE(common::is_ok(next.open_stream(elm327::sim::start_demo(), elm327::AdapterOptions{})));
    CHECK(adapter.execute("ATI", response) == common::Status::kDisconnected);
    REQUIRE(common::is_ok(next.execute("ATI", response, common::Deadline::after(5s))));
    CHECK(test::string_of(response.line(0U)) == "ELM327 v1.5 (simulator)");
    elm327::sim::stop_demo();
    CHECK(next.execute("ATI", response) == common::Status::kDisconnected);
}

TEST_CASE(adapter_over_tcp) {
    elm327::sim::FakeElm327 fake{elm327::sim::example_vehicle(), elm327::sim::Behaviour{},
                                 elm327::sim::Transport::kTcp};
    REQUIRE(fake.valid());
    CHECK(fake.device_path().rfind("tcp:127.0.0.1:", 0U) == 0U);
    elm327::Adapter adapter{};
    REQUIRE(open(adapter, fake));
    REQUIRE(common::is_ok(adapter.initialize('0')));
    elm327::Response response{};
    REQUIRE(common::is_ok(adapter.execute("ATRV", response, common::Deadline::after(5s))));
    CHECK(test::string_of(response.line(0U)) == "12.6V");
    CHECK(open(adapter, fake) == false);  // Open already.

    // Closed and opened again, as a person reconnects; then the adapter goes away.
    CHECK(common::is_ok(adapter.close()));
    REQUIRE(open(adapter, fake));
    REQUIRE(common::is_ok(adapter.execute("ATRV", response, common::Deadline::after(5s))));
    const std::string device{fake.device_path()};
    fake.hang_up();
    CHECK(adapter.execute("ATI", response) == common::Status::kDisconnected);
    CHECK(common::is_ok(adapter.close()));

    // Nothing listens at the port any more.
    CHECK(adapter.open(device.c_str(), elm327::AdapterOptions{}) == common::Status::kDisconnected);
}

TEST_CASE(adapter_tcp_addresses) {
    elm327::Adapter adapter{};
    for (const char* const device : {"tcp:", "tcp:192.168.0.10", "tcp:192.168.0.10:", "tcp::35000",
                                     "tcp:192.168.0.10:0", "tcp:192.168.0.10:65536", "tcp:192.168.0.10:3x",
                                     "tcp:obd.example:35000", "tcp:[::1:35000", "tcp:192.168.0.300:35000"}) {
        CHECK(adapter.open(device, elm327::AdapterOptions{}) == common::Status::kInvalidArgument);
    }
    CHECK(!adapter.is_open());

    // An address that does not answer: the wait ends at the cancel descriptor, or the network says
    // at once that it is unreachable.
    test::Event event{};
    REQUIRE(event.valid());
    event.signal();
    adapter.set_cancel_descriptor(event.descriptor());
    const common::TimePoint start{common::MonotonicClock::now()};
    const common::Status cancelled{adapter.open("tcp:192.0.2.1:35000", elm327::AdapterOptions{})};
    CHECK((cancelled == common::Status::kInterrupted) || (cancelled == common::Status::kDisconnected));
    adapter.set_cancel_descriptor(common::kInvalidNativeHandle);
    elm327::AdapterOptions quick{};
    quick.timeouts.link = 50ms;
    const common::Status timed_out{adapter.open("tcp:[2001:db8::1]:35000", quick)};
    CHECK((timed_out == common::Status::kTimeout) || (timed_out == common::Status::kDisconnected));
    CHECK((common::MonotonicClock::now() - start) < 2s);
    CHECK(!adapter.is_open());
}

TEST_CASE(adapter_cancel_over_a_stream) {
    elm327::sim::Behaviour behaviour{};
    behaviour.silent = true;
    elm327::sim::FakeElm327 fake{elm327::sim::example_vehicle(), behaviour, elm327::sim::Transport::kSocket};
    REQUIRE(fake.valid());
    elm327::Adapter adapter{};
    REQUIRE(common::is_ok(adapter.open_stream(fake.take_stream(), elm327::AdapterOptions{})));
    elm327::Response response{};
    CHECK(adapter.execute("ATI", response, common::Deadline::after(50ms)) == common::Status::kTimeout);
    test::Event event{};
    REQUIRE(event.valid());
    adapter.set_cancel_descriptor(event.descriptor());
    const common::TimePoint start{common::MonotonicClock::now()};
    std::thread canceller{[&event]() {
        std::this_thread::sleep_for(30ms);
        event.signal();
    }};
    CHECK(adapter.execute("ATI", response, common::Deadline::after(5s)) == common::Status::kInterrupted);
    canceller.join();
    CHECK((common::MonotonicClock::now() - start) < 2s);
}
#endif
