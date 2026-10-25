#include <signal.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/controller/controller.hpp"
#include "elm327/controller/explain.hpp"
#include "elm327/controller/signals.hpp"
#include "elm327/controller/timing.hpp"
#include "elm327/obd/dtc.hpp"
#include "elm327/obd/vin.hpp"
#include "sim/fake_elm327.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;

namespace {

elm327::ConnectionSettings settings_for(const elm327::sim::FakeElm327& fake) {
    elm327::ConnectionSettings settings{};
    static_cast<void>(settings.device.assign(fake.device_path()));
    return settings;
}

std::string text_of(const elm327::PidReading& reading, const bool with_unit = true) {
    std::array<char, elm327::kMaxReadingText> text{};
    const common::Result<std::size_t> length{elm327::format_reading(reading, with_unit, ' ', text)};
    return length.has_value() ? std::string{text.data(), length.value()} : std::string{"(error)"};
}

std::size_t count_of(const std::vector<std::string>& commands, const std::string& command) {
    std::size_t count{0U};
    for (const std::string& sent : commands) {
        count += (sent == command) ? 1U : 0U;
    }
    return count;
}

std::string reason_of(const elm327::Explanation& explanation) {
    std::string text{test::string_of(explanation.reason)};
    if (!explanation.advice.empty()) {
        text += " (" + std::string{explanation.advice} + ")";
    }
    return text;
}

}  // namespace

TEST_CASE(controller_settings_and_protocols) {
    const elm327::ConnectionSettings settings{};
    CHECK(settings.device.view() == "/dev/ttyUSB0");
    CHECK(settings.baud_rate == common::BaudRate::k38400);
    CHECK(settings.protocol == '0');

    CHECK(elm327::parse_protocol(test::span_of("6")).value() == '6');
    CHECK(elm327::parse_protocol(test::span_of("c")).value() == 'C');
    CHECK(!elm327::parse_protocol(test::span_of("D")).has_value());
    CHECK(!elm327::parse_protocol(test::span_of("10")).has_value());
    CHECK(!elm327::parse_protocol(test::span_of("")).has_value());
    CHECK(std::string{elm327::protocol_name('0')} == "automatic");
    CHECK(std::string{elm327::protocol_name('6')} == "ISO 15765-4 CAN (11 bit ID, 500 kbaud)");
    CHECK(std::string{elm327::protocol_name('Z')} == "unknown");
    CHECK(elm327::protocols() == "0123456789ABC");
}

TEST_CASE(controller_connects_and_reads) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    elm327::Controller controller{};
    CHECK(!controller.connected());
    REQUIRE(common::is_ok(controller.connect(settings_for(fake))));
    CHECK(controller.connected());
    CHECK(controller.connect_failure() == elm327::ConnectFailure::kNone);
    CHECK(test::string_of(controller.identity()) == "ELM327 v1.5");
    CHECK(controller.settings().device.view() == fake.device_path());

    elm327::AdapterText text{};
    REQUIRE(common::is_ok(controller.query("ATRV", text)));
    CHECK(text.view() == "12.6V");
    CHECK(controller.query("ATXYZ", text) == common::Status::kNotSupported);  // The adapter answers "?".
    CHECK(text.empty());

    CHECK(!controller.knows_supported_pids());
    REQUIRE(common::is_ok(controller.ensure_supported_pids()));
    CHECK(controller.knows_supported_pids());
    CHECK(controller.supported_pids().contains(std::uint8_t{0x0CU}));
    CHECK(controller.supported_pids().contains(std::uint8_t{0xA6U}));  // From the last range.

    const elm327::PidReading speed{controller.read_pid(0x0CU)};
    CHECK(common::is_ok(speed.status));
    CHECK(speed.pid == 0x0CU);
    CHECK(test::vector_of(common::Span<const std::uint8_t>{speed.data.data(), speed.data.size()}) ==
          (std::vector<std::uint8_t>{0x1AU, 0xF8U}));
    REQUIRE(speed.value.has_value());
    CHECK(speed.value.value().units() == 1'726'000);
    CHECK(text_of(speed) == "1726.00 rpm");
    CHECK(text_of(speed, false) == "1726.00");

    // A PID without a single value shows its bytes.
    const elm327::PidReading monitors{controller.read_pid(0x01U)};
    CHECK(common::is_ok(monitors.status));
    CHECK(monitors.value.status() == common::Status::kNotSupported);
    CHECK(text_of(monitors) == "82 07 E5 00");

    const elm327::PidReading missing{controller.read_pid(0x0AU)};
    CHECK(missing.status == common::Status::kNotFound);
    CHECK(reason_of(elm327::explain_failure(missing.status, controller.last_response())) == "NO DATA");

    const common::Result<elm327::MilStatus> mil{controller.read_mil_status()};
    REQUIRE(mil.has_value());
    CHECK(mil.value().on);
    CHECK(mil.value().dtc_count == 2U);

    elm327::DtcList dtcs{};
    REQUIRE(common::is_ok(controller.read_dtcs(elm327::DtcKind::kStored, dtcs)));
    CHECK(dtcs.size() == 2U);
    elm327::Vin vin{};
    REQUIRE(common::is_ok(controller.read_vin(vin)));
    CHECK(vin.view() == "1D4GP00R55B123456");

    elm327::Response response{};
    REQUIRE(common::is_ok(controller.execute("010D", response)));
    CHECK(test::string_of(response.line(0U)) == "41 0D 2A");

    controller.disconnect();
    CHECK(!controller.connected());
    CHECK(!controller.knows_supported_pids());
    CHECK(controller.identity().empty());
}

TEST_CASE(controller_reads_the_supported_pids_once) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    elm327::Controller controller{};
    REQUIRE(common::is_ok(controller.connect(settings_for(fake))));
    REQUIRE(common::is_ok(controller.connect_vehicle()));
    CHECK(!controller.knows_supported_pids());  // Only the first range.
    CHECK(controller.supported_pids().contains(std::uint8_t{0x0CU}));
    REQUIRE(common::is_ok(controller.ensure_supported_pids()));
    REQUIRE(common::is_ok(controller.ensure_supported_pids()));
    REQUIRE(common::is_ok(controller.connect_vehicle()));  // Known: nothing is sent.
    CHECK(count_of(fake.commands(), "0100") == 2U);         // connect_vehicle() and the first ensure.
    CHECK(count_of(fake.commands(), "01A0") == 1U);

    // An AT command may have reset the adapter: the PIDs are read again when needed.
    elm327::Response response{};
    REQUIRE(common::is_ok(controller.execute("ATZ", response)));
    CHECK(!controller.knows_supported_pids());
    REQUIRE(common::is_ok(controller.ensure_supported_pids()));
    CHECK(count_of(fake.commands(), "0100") == 3U);
    // An OBD request does not.
    REQUIRE(common::is_ok(controller.execute("010C", response)));
    CHECK(controller.knows_supported_pids());

    // read_supported_pids() always asks again.
    REQUIRE(common::is_ok(controller.read_supported_pids()));
    CHECK(count_of(fake.commands(), "0100") == 4U);

    // A new connection starts over.
    REQUIRE(common::is_ok(controller.connect(settings_for(fake))));
    CHECK(!controller.knows_supported_pids());
    CHECK(count_of(fake.commands(), "ATZ") == 3U);
}

TEST_CASE(controller_connect_failures) {
    elm327::Controller controller{};
    elm327::ConnectionSettings settings{};
    REQUIRE(settings.device.assign("/nonexistent/ttyUSB9"));
    CHECK(controller.connect(settings) == common::Status::kNotFound);
    CHECK(controller.connect_failure() == elm327::ConnectFailure::kOpen);
    CHECK(!controller.connected());
    CHECK(reason_of(elm327::explain_connect_failure(controller.connect_failure(), common::Status::kNotFound)) ==
          "no such device (choose the adapter's serial device)");

    REQUIRE(settings.device.assign("/dev/null"));
    CHECK(controller.connect(settings) == common::Status::kInvalidArgument);
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kInvalidArgument)) == "not a serial device");

    // A device that never answers, like an adapter at another baud rate.
    elm327::sim::Behaviour silent{};
    silent.silent = true;
    elm327::sim::FakeElm327 fake{elm327::sim::example_vehicle(), silent};
    REQUIRE(fake.valid());
    settings = settings_for(fake);
    settings.timeouts.reset = 100ms;
    CHECK(controller.connect(settings) == common::Status::kTimeout);
    CHECK(controller.connect_failure() == elm327::ConnectFailure::kNoAnswer);
    CHECK(!controller.connected());
    CHECK(reason_of(elm327::explain_connect_failure(elm327::ConnectFailure::kNoAnswer, common::Status::kTimeout)) ==
          "the adapter does not answer (check the device and the baud rate)");
    CHECK(reason_of(elm327::explain_connect_failure(elm327::ConnectFailure::kInitialize,
                                                    common::Status::kNotSupported)) ==
          "the adapter rejected its configuration");
}

TEST_CASE(controller_notices_a_lost_device) {
    elm327::sim::Vehicle vehicle{elm327::sim::example_vehicle()};
    vehicle.ignition_on = false;
    elm327::sim::FakeElm327 fake{vehicle};
    REQUIRE(fake.valid());
    elm327::Controller controller{};
    REQUIRE(common::is_ok(controller.connect(settings_for(fake))));

    // "UNABLE TO CONNECT" is kDisconnected as well, but the adapter is still there.
    CHECK(controller.connect_vehicle() == common::Status::kDisconnected);
    CHECK(controller.connected());
    CHECK(reason_of(elm327::explain_failure(common::Status::kDisconnected, controller.last_response())) ==
          "UNABLE TO CONNECT (is the ignition on?)");

    fake.hang_up();
    const elm327::PidReading reading{controller.read_pid(0x0CU)};
    CHECK(reading.status == common::Status::kDisconnected);
    CHECK(!controller.connected());
    CHECK(reason_of(elm327::explain_failure(reading.status, controller.last_response())) == "disconnected");
}

TEST_CASE(controller_explanations) {
    const elm327::Response none{};
    CHECK(reason_of(elm327::explain_failure(common::Status::kInterrupted, none)) == "interrupted");
    CHECK(reason_of(elm327::explain_failure(common::Status::kTimeout, none)) == "timed out");
    CHECK(reason_of(elm327::explain_failure(common::Status::kInvalidArgument, none)) == "unexpected reply");
    const elm327::Response bus{test::make_response({"SEARCHING...", "CAN ERROR"})};
    CHECK(reason_of(elm327::explain_failure(common::Status::kSystemError, bus)) == "CAN ERROR");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kPermissionDenied)) ==
          "permission denied (add your user to the dialout group)");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kResourceLimit)) == "in use by another program");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kNotSupported)) ==
          "the device does not accept the line settings");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kSystemError)) == "system error");
}

TEST_CASE(controller_format_reading) {
    elm327::PidReading reading{};
    reading.pid = 0xFEU;  // Unknown: the bytes.
    REQUIRE(reading.data.push_back(0x12U));
    REQUIRE(reading.data.push_back(0xABU));
    CHECK(text_of(reading) == "12 AB");
    std::array<char, 4U> small{};
    CHECK(elm327::format_reading(reading, true, ' ', small).status() == common::Status::kOutOfRange);
    std::array<char, elm327::kMaxReadingText> text{};
    CHECK(elm327::format_reading(reading, true, '\0', text).value() == 4U);

    reading.pid = 0x05U;  // Coolant: -40 degC.
    reading.value = common::Result<elm327::Measurement>::success(elm327::Measurement::from_units(-40'000));
    CHECK(text_of(reading) == "-40 degC");
    CHECK(text_of(reading, false) == "-40");
}

TEST_CASE(timing_next_activation_and_wait) {
    const common::TimePoint start{common::TimePoint{} + 10s};
    CHECK(elm327::next_activation(start, 100ms, start) == (start + 100ms));
    CHECK(elm327::next_activation(start, 100ms, start + 50ms) == (start + 100ms));
    // Activations missed during a slow read are skipped, on the same grid.
    CHECK(elm327::next_activation(start, 100ms, start + 350ms) == (start + 400ms));
    CHECK(elm327::next_activation(start, 100ms, start + 400ms) == (start + 500ms));

    const common::TimePoint before{common::MonotonicClock::now()};
    CHECK(common::is_ok(elm327::wait_until(-1, common::Deadline::after(30ms))));
    CHECK((common::MonotonicClock::now() - before) >= 30ms);
    test::Event event{};
    REQUIRE(event.descriptor() >= 0);
    CHECK(common::is_ok(elm327::wait_until(event.descriptor(), common::Deadline::after(10ms))));
    event.signal();
    CHECK(elm327::wait_until(event.descriptor(), common::Deadline::after(5s)) == common::Status::kInterrupted);
}

TEST_CASE(signals_take_stop_requests) {
    test::Event event{};
    REQUIRE(event.descriptor() >= 0);
    CHECK(elm327::take_stop_request(event.descriptor()) == 0);
    event.signal();
    event.signal();
    CHECK(elm327::take_stop_request(event.descriptor()) == SIGINT);
    CHECK(elm327::take_stop_request(event.descriptor()) == 0);  // Taken.
    CHECK(elm327::wait_until(event.descriptor(), common::Deadline::after(1ms)) == common::Status::kOk);
    CHECK(elm327::take_stop_request(-1) == 0);

    CHECK(elm327::ends_program(SIGTERM));
    CHECK(elm327::ends_program(SIGHUP));
    CHECK(elm327::ends_program(SIGPIPE));
    CHECK(!elm327::ends_program(SIGINT));

    // Real signals, on a thread of their own: the blocked mask stays with that thread.
    std::int32_t first{-1};
    std::int32_t second{-1};
    std::int32_t received{-1};
    std::thread signalled{[&first, &second, &received]() {
        elm327::StopSignals stop{};
        if (common::is_ok(stop.open())) {
            static_cast<void>(::raise(SIGINT));
            static_cast<void>(::raise(SIGTERM));
            first = elm327::take_stop_request(stop.descriptor());  // SIGTERM wins: it ends a program.
            static_cast<void>(::raise(SIGINT));
            second = elm327::take_stop_request(stop.descriptor());
            received = stop.received();  // Nothing left.
        }
    }};
    signalled.join();
    CHECK(first == SIGTERM);
    CHECK(second == SIGINT);
    CHECK(received == 0);
}

TEST_CASE(controller_clears_dtcs_when_writable) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    elm327::Controller controller{};
    REQUIRE(common::is_ok(controller.connect(settings_for(fake))));
    REQUIRE(common::is_ok(controller.connect_vehicle()));
    CHECK(controller.access_policy() == elm327::AccessPolicy::kReadWrite);  // The default.

    elm327::DtcList before{};
    REQUIRE(common::is_ok(controller.read_dtcs(elm327::DtcKind::kStored, before)));
    CHECK(!before.empty());

    CHECK(common::is_ok(controller.clear_dtcs()));
    elm327::DtcList after{};
    REQUIRE(common::is_ok(controller.read_dtcs(elm327::DtcKind::kStored, after)));
    CHECK(after.empty());
    CHECK(count_of(fake.commands(), "04") == 1U);  // The clear request reached the vehicle once.
}

TEST_CASE(controller_read_only_blocks_every_write) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    elm327::Controller controller{};
    REQUIRE(common::is_ok(controller.connect(settings_for(fake))));
    controller.set_access_policy(elm327::AccessPolicy::kReadOnly);
    CHECK(controller.read_only());

    // clear_dtcs() is refused before anything is sent.
    CHECK(controller.clear_dtcs() == common::Status::kPermissionDenied);

    // A raw OBD clear and UDS writes are refused, and the response is left empty.
    elm327::Response response{};
    CHECK(controller.execute("04", response) == common::Status::kPermissionDenied);
    CHECK(response.empty());
    CHECK(controller.execute("2EF19001", response) == common::Status::kPermissionDenied);
    CHECK(controller.execute("3101FF00", response) == common::Status::kPermissionDenied);
    CHECK(controller.execute("2701", response) == common::Status::kPermissionDenied);

    // Reads still work: an AT command, an OBD read and a UDS read.
    CHECK(common::is_ok(controller.execute("ATRV", response)));
    CHECK(common::is_ok(controller.execute("22F190", response)));

    // The decisive check: no write service ever reached the simulated vehicle.
    for (const std::string& command : fake.commands()) {
        const std::string service{command.substr(0U, 2U)};
        CHECK(service != "04");
        CHECK(service != "2E");
        CHECK(service != "31");
        CHECK(service != "27");
        CHECK(service != "2F");
    }
}

TEST_CASE(controller_read_only_allows_reads) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    elm327::Controller controller{};
    REQUIRE(common::is_ok(controller.connect(settings_for(fake))));
    controller.set_access_policy(elm327::AccessPolicy::kReadOnly);
    REQUIRE(common::is_ok(controller.ensure_supported_pids()));

    const elm327::PidReading speed{controller.read_pid(0x0DU)};
    CHECK(common::is_ok(speed.status));
    elm327::DtcList dtcs{};
    CHECK(common::is_ok(controller.read_dtcs(elm327::DtcKind::kStored, dtcs)));
    elm327::Vin vin{};
    CHECK(common::is_ok(controller.read_vin(vin)));
}
