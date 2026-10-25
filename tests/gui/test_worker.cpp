#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "elm327/obd/dtc.hpp"
#include "gui/worker.hpp"
#include "sim/fake_elm327.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace std::chrono_literals;

using elm327::gui::Activity;
using elm327::gui::Request;
using elm327::gui::RequestKind;
using elm327::gui::State;
using elm327::gui::Worker;

namespace {

/// Waits until the worker publishes a State that satisfies @p condition, for at most 10 s.
template <typename Condition>
bool wait_for(Worker& worker, Condition condition) {
    const common::TimePoint deadline{common::MonotonicClock::now() + 10s};
    bool met{condition(worker.state())};
    while ((!met) && (common::MonotonicClock::now() < deadline)) {
        std::this_thread::sleep_for(2ms);
        static_cast<void>(worker.update_state());
        met = condition(worker.state());
    }
    return met;
}

bool idle(const State& state) {
    return state.activity == Activity::kIdle;
}

Request connect_request(const std::string& device) {
    Request request{};
    request.kind = RequestKind::kConnect;
    static_cast<void>(request.settings.device.assign(device));
    return request;
}

/// The trace lines published so far.
std::vector<std::string> trace_of(Worker& worker) {
    std::vector<std::string> lines{};
    elm327::gui::TraceLine line{};
    while (worker.take_trace(line)) {
        lines.push_back(test::string_of(line.text.text()));
    }
    return lines;
}

bool has_line(const std::vector<std::string>& lines, const std::string& text) {
    bool found{false};
    for (const std::string& line : lines) {
        found = found || (line == text);
    }
    return found;
}

void count_notification(void* const context) noexcept {
    static_cast<std::atomic<std::uint32_t>*>(context)->fetch_add(1U);
}

}  // namespace

TEST_CASE(worker_connects_and_reads) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    std::atomic<std::uint32_t> notifications{0U};
    REQUIRE(common::is_ok(worker->start(&count_notification, &notifications)));
    CHECK(worker->start(nullptr, nullptr) == common::Status::kAlreadyRunning);
    REQUIRE(worker->update_state());
    CHECK(!worker->state().connected);

    // Connecting also reads the adapter and the supported PIDs.
    REQUIRE(worker->post(connect_request(fake.device_path())));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.pids_known && idle(state); }));
    const State& state{worker->state()};
    CHECK(state.connected);
    CHECK(state.identity.view() == "ELM327 v1.5");
    CHECK(state.description.view() == "OBDII to RS232 Interpreter");
    CHECK(state.voltage.view() == "12.6V");
    CHECK(state.protocol.view() == "AUTO, ISO 15765-4 (CAN 11/500)");
    CHECK(state.supported_pids.contains(std::uint8_t{0x0CU}));
    CHECK(state.message.view() == "23 PIDs supported.");
    CHECK(!state.message_failed);
    CHECK(notifications.load() > 0U);
    const std::vector<std::string> lines{trace_of(*worker)};
    CHECK(has_line(lines, "ATZ"));
    CHECK(has_line(lines, "ELM327 v1.5"));
    CHECK(has_line(lines, "Connected to ELM327 v1.5 on " + fake.device_path() + "."));

    REQUIRE(worker->post(RequestKind::kReadDtcs));
    REQUIRE(wait_for(*worker, [](const State& current) { return current.dtcs_read && idle(current); }));
    CHECK(worker->state().mil_known);
    CHECK(worker->state().mil.on);
    CHECK(worker->state().dtcs[0U].kind == elm327::DtcKind::kStored);
    CHECK(worker->state().dtcs[0U].codes.size() == 2U);
    CHECK(worker->state().dtcs[1U].codes.size() == 1U);
    CHECK(worker->state().dtcs[2U].codes.size() == 1U);
    CHECK(worker->state().message.view() == "Trouble codes read: 2 stored, 1 pending, 1 permanent.");

    REQUIRE(worker->post(RequestKind::kReadVin));
    REQUIRE(wait_for(*worker, [](const State& current) { return current.vin_read && idle(current); }));
    CHECK(worker->state().vin.view() == "1D4GP00R55B123456");

    // Values of the supported PIDs among those asked for.
    Request values{};
    values.kind = RequestKind::kReadValues;
    values.pids = common::ByteSet{}.with(std::uint8_t{0x0CU}).with(std::uint8_t{0x0DU}).with(std::uint8_t{0x0AU});
    REQUIRE(worker->post(values));
    elm327::gui::Sample sample{};
    const common::TimePoint deadline{common::MonotonicClock::now() + 10s};
    bool taken{false};
    while ((!taken) && (common::MonotonicClock::now() < deadline)) {
        taken = worker->take_sample(sample);
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(taken);
    CHECK(!sample.monitored);
    REQUIRE(sample.readings.size() == 2U);
    CHECK(sample.readings[0U].pid == 0x0CU);
    CHECK(sample.readings[0U].value.value().units() == 1'726'000);
    CHECK(sample.readings[1U].pid == 0x0DU);

    REQUIRE(worker->post(RequestKind::kDisconnect));
    REQUIRE(wait_for(*worker, [](const State& current) { return !current.connected; }));
    CHECK(!worker->state().pids_known);
    CHECK(worker->state().message.view() == "Disconnected.");
    worker->stop();
    CHECK(!worker->post(RequestKind::kReadVin));  // Not running.
}

TEST_CASE(worker_monitors_until_cancelled) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(common::is_ok(worker->start(nullptr, nullptr)));
    REQUIRE(worker->post(connect_request(fake.device_path())));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.pids_known && idle(state); }));

    Request monitor{};
    monitor.kind = RequestKind::kStartMonitor;
    monitor.pids = common::ByteSet{}.with(std::uint8_t{0x0CU}).with(std::uint8_t{0x05U});
    monitor.interval = 20ms;
    REQUIRE(worker->post(monitor));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.monitoring; }));
    CHECK(worker->state().activity == Activity::kMonitoring);
    CHECK(worker->state().interval == 20ms);

    // Samples arrive on the grid, with the PIDs in ascending order.
    std::vector<elm327::gui::Sample> samples{};
    const common::TimePoint deadline{common::MonotonicClock::now() + 10s};
    while ((samples.size() < 5U) && (common::MonotonicClock::now() < deadline)) {
        elm327::gui::Sample sample{};
        if (worker->take_sample(sample)) {
            samples.push_back(sample);
        } else {
            std::this_thread::sleep_for(2ms);
        }
    }
    REQUIRE(samples.size() == 5U);
    for (const elm327::gui::Sample& sample : samples) {
        CHECK(sample.monitored);
        REQUIRE(sample.readings.size() == 2U);
        CHECK(sample.readings[0U].pid == 0x05U);
        CHECK(sample.readings[1U].pid == 0x0CU);
    }
    CHECK(samples[4U].time > samples[0U].time);

    // cancel() stops the monitor; a request posted afterwards runs.
    worker->cancel();
    REQUIRE(worker->post(RequestKind::kReadVin));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.vin_read && idle(state); }));
    CHECK(!worker->state().monitoring);
    CHECK(worker->state().connected);
    CHECK(has_line(trace_of(*worker), "Monitoring stopped."));

    // kStopMonitor stops it too.
    REQUIRE(worker->post(monitor));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.monitoring; }));
    REQUIRE(worker->post(RequestKind::kStopMonitor));
    REQUIRE(wait_for(*worker, [](const State& state) { return (!state.monitoring) && idle(state); }));
    worker->stop();
}

TEST_CASE(worker_cancel_drops_queued_requests) {
    // A slow protocol search: the first OBD request takes 500 ms.
    elm327::sim::Behaviour behaviour{};
    behaviour.search_delay = 500ms;
    elm327::sim::FakeElm327 fake{elm327::sim::example_vehicle(), behaviour};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(common::is_ok(worker->start(nullptr, nullptr)));
    REQUIRE(worker->post(connect_request(fake.device_path())));
    REQUIRE(worker->post(RequestKind::kReadDtcs));  // Queued behind the connection.
    // Cancel while the adapter searches for the protocol, after the first OBD request.
    bool searching{false};
    const common::TimePoint deadline{common::MonotonicClock::now() + 10s};
    while ((!searching) && (common::MonotonicClock::now() < deadline)) {
        elm327::gui::TraceLine line{};
        if (worker->take_trace(line)) {
            searching = line.text.view() == "0100";
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    REQUIRE(searching);
    std::this_thread::sleep_for(50ms);
    worker->cancel();
    REQUIRE(wait_for(*worker, [](const State& state) { return state.connected && idle(state); }));
    std::this_thread::sleep_for(100ms);
    static_cast<void>(worker->update_state());
    // The search was interrupted, and the queued request dropped.
    CHECK(!worker->state().dtcs_read);
    CHECK(!worker->state().pids_known);
    CHECK(worker->state().message_failed);
    CHECK(worker->state().message.view() == "Connecting to the vehicle: interrupted");
    worker->stop();
}

TEST_CASE(worker_reports_failures) {
    const auto worker = std::make_unique<Worker>();
    REQUIRE(common::is_ok(worker->start(nullptr, nullptr)));
    REQUIRE(worker->post(RequestKind::kReadDtcs));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.message_count == 1U; }));
    CHECK(worker->state().message_failed);
    CHECK(worker->state().message.view() == "Not connected: connect to the adapter first.");

    REQUIRE(worker->post(connect_request("/nonexistent/ttyUSB9")));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.message_count == 2U && idle(state); }));
    CHECK(!worker->state().connected);
    CHECK(worker->state().message.view() ==
          "Cannot connect to /nonexistent/ttyUSB9: no such device (choose the adapter's serial device)");

    // The device goes away while connected.
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    REQUIRE(worker->post(connect_request(fake.device_path())));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.pids_known && idle(state); }));
    fake.hang_up();
    REQUIRE(worker->post(RequestKind::kReadVin));
    REQUIRE(wait_for(*worker, [](const State& state) { return (!state.connected) && idle(state); }));
    CHECK(worker->state().message.view() == "The connection to the adapter was lost.");
    CHECK(!worker->state().pids_known);
    worker->stop();
    worker->stop();  // Twice is fine.
}
