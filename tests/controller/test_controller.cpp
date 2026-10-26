#include <signal.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/controller/bridged_link.hpp"
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

#if !defined(_WIN32)
/// A runtime for open_bridged_link(): answers after a while, or never, and keeps the end it got.
struct Runtime final {
    int answer{0};  ///< The status byte; -1 closes the end without one, -2 never answers.
    std::chrono::milliseconds delay{0};
    std::string address{};
    int end{-1};
    std::thread thread{};

    ~Runtime() {
        if (thread.joinable()) {
            thread.join();
        }
        if (end >= 0) {
            static_cast<void>(::close(end));
        }
    }
};

void hand_to_runtime(const char* const address, const common::NativeHandle end, void* const context) noexcept {
    Runtime* const runtime{static_cast<Runtime*>(context)};
    runtime->address = address;
    runtime->end = end;
    runtime->thread = std::thread{[runtime]() {
        std::this_thread::sleep_for(runtime->delay);
        if (runtime->answer >= 0) {
            const std::uint8_t byte{static_cast<std::uint8_t>(runtime->answer)};
            const ssize_t written{::write(runtime->end, &byte, 1U)};
            static_cast<void>(written);
        } else if (runtime->answer == -1) {
            static_cast<void>(::close(runtime->end));
            runtime->end = -1;
        } else {
            // Never answers.
        }
    }};
}

/// The links of a LinkOpener for the tests: "sim" is the simulator's socket, "bt:..." fails with
/// failure, and anything else is a serial device.
struct Links final {
    elm327::sim::FakeElm327* fake{nullptr};
    common::Status failure{common::Status::kNotFound};
    int calls{0};
    common::NativeHandle cancel{common::kInvalidNativeHandle};
    common::Duration remaining{};
};

common::Result<common::NativeHandle> open_link(const char* const device, const common::Deadline& deadline,
                                               const common::NativeHandle cancel, void* const context) noexcept {
    Links* const links{static_cast<Links*>(context)};
    ++links->calls;
    links->cancel = cancel;
    links->remaining = deadline.remaining();
    const std::string name{device};
    common::Result<common::NativeHandle> result{common::Result<common::NativeHandle>::failure(
        common::Status::kNotSupported)};
    if (name == "sim") {
        result = common::Result<common::NativeHandle>::success(links->fake->take_stream());
    } else if (name.rfind("bt:", 0U) == 0U) {
        result = common::Result<common::NativeHandle>::failure(links->failure);
    } else {
        // A serial device.
    }
    return result;
}
#endif

}  // namespace

TEST_CASE(controller_settings_and_protocols) {
    const elm327::ConnectionSettings settings{};
    CHECK(settings.device.view() == elm327::kDefaultDevice);
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
    REQUIRE(settings.device.assign(test::kMissingDevice));
    CHECK(controller.connect(settings) == common::Status::kNotFound);
    CHECK(controller.connect_failure() == elm327::ConnectFailure::kOpen);
    CHECK(!controller.connected());
    CHECK(reason_of(elm327::explain_connect_failure(controller.connect_failure(), common::Status::kNotFound)) ==
          "no such device (choose the adapter's serial device)");

    REQUIRE(settings.device.assign(test::kNotSerialDevice));
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

#if !defined(_WIN32)
TEST_CASE(controller_opens_links) {
    elm327::sim::FakeElm327 link{elm327::sim::example_vehicle(), elm327::sim::Behaviour{},
                                 elm327::sim::Transport::kSocket};
    elm327::sim::FakeElm327 device{};
    REQUIRE(link.valid() && device.valid());
    Links links{};
    links.fake = &link;
    test::Event event{};
    REQUIRE(event.valid());
    elm327::Controller controller{};
    controller.set_cancel_descriptor(event.descriptor());
    controller.set_link_opener(&open_link, &links);

    elm327::ConnectionSettings settings{};
    REQUIRE(settings.device.assign("sim"));
    settings.timeouts.link = 2s;
    REQUIRE(common::is_ok(controller.connect(settings)));
    CHECK(controller.connected());
    CHECK(test::string_of(controller.identity()) == "ELM327 v1.5");
    CHECK(links.calls == 1);
    CHECK(links.cancel == event.descriptor());
    CHECK((links.remaining > 1s) && (links.remaining <= 2s));
    CHECK(common::is_ok(controller.connect_vehicle()));
    CHECK(text_of(controller.read_pid(0x0DU)) == "42 km/h");

    // The device that is not a link is a serial device.
    REQUIRE(common::is_ok(controller.connect(settings_for(device))));
    CHECK(links.calls == 2);
    CHECK(controller.connected());

    // A link that cannot be opened fails like a device, and the connection before it is closed.
    REQUIRE(settings.device.assign("bt:00:11:22:33:44:55"));
    links.failure = common::Status::kPermissionDenied;
    CHECK(controller.connect(settings) == common::Status::kPermissionDenied);
    CHECK(controller.connect_failure() == elm327::ConnectFailure::kOpen);
    CHECK(!controller.connected());

    // Without the opener, the link is taken for a serial device.
    controller.set_link_opener(nullptr, nullptr);
    CHECK(controller.connect(settings) == common::Status::kNotFound);
    CHECK(links.calls == 3);
}
#endif

#if !defined(_WIN32)
TEST_CASE(bridged_links) {
    {
        Runtime runtime{};
        runtime.delay = 20ms;
        const common::Result<common::NativeHandle> link{
            elm327::open_bridged_link("00:1D:A5:68:98:8B", &hand_to_runtime, &runtime, common::Deadline::after(2s),
                                      common::kInvalidNativeHandle)};
        REQUIRE(link.has_value());
        runtime.thread.join();
        CHECK(runtime.address == "00:1D:A5:68:98:8B");
        // From now on the bytes pass, both ways.
        const char hello{'>'};
        CHECK(::write(runtime.end, &hello, 1U) == 1);
        char received{'\0'};
        CHECK(::read(link.value(), &received, 1U) == 1);
        CHECK(received == '>');
        static_cast<void>(::close(link.value()));
    }
    const std::array<std::pair<int, common::Status>, 5U> answers{{
        {1, common::Status::kNotFound},
        {2, common::Status::kPermissionDenied},
        {3, common::Status::kNotRunning},
        {4, common::Status::kDisconnected},
        {-1, common::Status::kDisconnected},  // Closed without an answer.
    }};
    for (const auto& [answer, status] : answers) {
        Runtime runtime{};
        runtime.answer = answer;
        CHECK(elm327::open_bridged_link("addr", &hand_to_runtime, &runtime, common::Deadline::after(2s),
                                        common::kInvalidNativeHandle)
                  .status() == status);
    }
    {
        Runtime runtime{};
        runtime.answer = -2;
        CHECK(elm327::open_bridged_link("addr", &hand_to_runtime, &runtime, common::Deadline::after(50ms),
                                        common::kInvalidNativeHandle)
                  .status() == common::Status::kTimeout);
        runtime.thread.join();
        // The runtime finds the link closed when it gives up.
        char byte{'\0'};
        CHECK(::read(runtime.end, &byte, 1U) == 0);
    }
    {
        Runtime runtime{};
        runtime.answer = -2;
        test::Event cancel{};
        REQUIRE(cancel.valid());
        cancel.signal();
        const common::TimePoint start{common::MonotonicClock::now()};
        CHECK(elm327::open_bridged_link("addr", &hand_to_runtime, &runtime, common::Deadline::after(5s),
                                        cancel.descriptor())
                  .status() == common::Status::kInterrupted);
        CHECK((common::MonotonicClock::now() - start) < 1s);
    }
    CHECK(elm327::open_bridged_link(nullptr, &hand_to_runtime, nullptr, common::Deadline::after(1s),
                                    common::kInvalidNativeHandle)
              .status() == common::Status::kInvalidArgument);
}
#endif

TEST_CASE(controller_explanations) {
    const elm327::Response none{};
    CHECK(reason_of(elm327::explain_failure(common::Status::kInterrupted, none)) == "interrupted");
    CHECK(reason_of(elm327::explain_failure(common::Status::kTimeout, none)) == "timed out");
    CHECK(reason_of(elm327::explain_failure(common::Status::kInvalidArgument, none)) == "unexpected reply");
    const elm327::Response bus{test::make_response({"SEARCHING...", "CAN ERROR"})};
    CHECK(reason_of(elm327::explain_failure(common::Status::kSystemError, bus)) == "CAN ERROR");
#if defined(__linux__) && !defined(__ANDROID__)
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kPermissionDenied)) ==
          "permission denied (add your user to the dialout group)");
#else
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kPermissionDenied)) == "permission denied");
#endif
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kTimeout)) == "timed out");

    // Each kind of device in its own words.
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kNotFound, "/dev/ttyUSB0")) ==
          "no such device (choose the adapter's serial device)");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kInvalidArgument, "tcp:obd:35000")) ==
          "not an IP address and port (e.g. tcp:192.168.0.10:35000)");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kTimeout, "tcp:192.168.0.10:35000")) ==
          "nothing answers at that address (join the adapter's Wi-Fi network first)");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kDisconnected, "tcp:192.168.0.10:35000")) ==
          "nothing answers at that address (join the adapter's Wi-Fi network first)");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kNotFound, "bt:00:1D:A5:68:98:8B")) ==
          "no such device (pair the adapter in the Bluetooth settings first)");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kNotRunning, "ble:6B1F")) ==
          "Bluetooth is off (turn Bluetooth on)");
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kDisconnected, "bt:00:1D:A5:68:98:8B")) ==
          "the adapter does not accept the connection (is it plugged in, and no other device connected to it?)");
#if defined(__APPLE__)
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kPermissionDenied, "ble:6B1F")) ==
          "permission denied (allow the app to use Bluetooth in the Settings)");
#else
    CHECK(reason_of(elm327::explain_open_failure(common::Status::kPermissionDenied, "bt:00:1D:A5:68:98:8B")) ==
          "permission denied (allow the app to connect to nearby devices)");
#endif
    CHECK(reason_of(elm327::explain_connect_failure(elm327::ConnectFailure::kOpen, common::Status::kTimeout,
                                                    "tcp:192.168.0.10:35000")) ==
          "nothing answers at that address (join the adapter's Wi-Fi network first)");
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
    CHECK(common::is_ok(elm327::wait_until(common::kInvalidNativeHandle, common::Deadline::after(30ms))));
    CHECK((common::MonotonicClock::now() - before) >= 30ms);
    test::Event event{};
    REQUIRE(event.valid());
    CHECK(common::is_ok(elm327::wait_until(event.descriptor(), common::Deadline::after(10ms))));
    event.signal();
    CHECK(elm327::wait_until(event.descriptor(), common::Deadline::after(5s)) == common::Status::kInterrupted);
}

TEST_CASE(signals_take_stop_requests) {
    test::Event event{};
    REQUIRE(event.valid());
    CHECK(elm327::take_stop_request(event.descriptor()) == 0);
    event.signal();
    event.signal();
    CHECK(elm327::take_stop_request(event.descriptor()) == SIGINT);
    CHECK(elm327::take_stop_request(event.descriptor()) == 0);  // Taken.
    CHECK(elm327::wait_until(event.descriptor(), common::Deadline::after(1ms)) == common::Status::kOk);
    CHECK(elm327::take_stop_request(common::kInvalidNativeHandle) == 0);

    CHECK(elm327::ends_program(SIGTERM));
#if defined(_WIN32)
    CHECK(elm327::ends_program(SIGBREAK));
#else
    CHECK(elm327::ends_program(SIGHUP));
    CHECK(elm327::ends_program(SIGPIPE));
#endif
    CHECK(!elm327::ends_program(SIGINT));
}

#if defined(_WIN32)
TEST_CASE(signals_console_handler) {
    elm327::StopSignals stop{};
    REQUIRE(common::is_ok(stop.open()));
    CHECK(stop.open() == common::Status::kInvalidArgument);
    // A process has one set of console handlers, so one StopSignals at a time.
    elm327::StopSignals other{};
    CHECK(other.open() == common::Status::kInvalidArgument);
    CHECK(other.descriptor() == common::kInvalidNativeHandle);
    // Nothing has arrived.
    CHECK(stop.received() == 0);
    CHECK(elm327::take_stop_request(stop.descriptor()) == 0);
    CHECK(elm327::wait_until(stop.descriptor(), common::Deadline::after(1ms)) == common::Status::kOk);
}
#else
TEST_CASE(signals_from_the_kernel) {
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
#endif

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
