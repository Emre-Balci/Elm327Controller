#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "elm327/obd/dtc.hpp"
#include "elm327/expert/feature.hpp"
#include "gui/features.hpp"
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

TEST_CASE(worker_read_only_refuses_writes) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(common::is_ok(worker->start(nullptr, nullptr)));

    Request connect{connect_request(fake.device_path())};
    connect.read_only = true;
    REQUIRE(worker->post(connect));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.pids_known && idle(state); }));
    CHECK(worker->state().read_only);

    // A UDS write typed into the terminal is refused before it is sent.
    Request raw{};
    raw.kind = RequestKind::kRaw;
    static_cast<void>(raw.command.append("2EF19001"));
    REQUIRE(worker->post(raw));
    REQUIRE(wait_for(*worker, [](const State& state) { return state.message_failed && idle(state); }));
    CHECK(test::string_of(worker->state().message.text()).find("read-only") != std::string::npos);

    worker->stop();
    for (const std::string& command : fake.commands()) {
        CHECK(command.substr(0U, 2U) != "2E");  // The write never reached the vehicle.
    }
}

namespace {

using elm327::gui::UdsResult;

/// Posts @p request and waits until the worker has told @p messages more messages and is idle.
bool post_and_wait(Worker& worker, const Request& request, const std::uint32_t messages) {
    static_cast<void>(worker.update_state());
    const std::uint32_t before{worker.state().message_count};
    return worker.post(request) && wait_for(worker, [before, messages](const State& state) {
               return (state.message_count >= (before + messages)) && idle(state);
           });
}

Request uds_request(const std::string& label, const std::string& command) {
    Request request{};
    request.kind = RequestKind::kUds;
    static_cast<void>(request.label.append(std::string_view{label}));
    static_cast<void>(request.command.append(std::string_view{command}));
    return request;
}

Request uds_write(const std::uint16_t did, const std::vector<std::uint8_t>& value) {
    Request request{};
    request.kind = RequestKind::kUdsWrite;
    request.did = did;
    for (const std::uint8_t byte : value) {
        static_cast<void>(request.value.push_back(byte));
    }
    return request;
}

Request raw_request(const std::string& command) {
    Request request{};
    request.kind = RequestKind::kRaw;
    static_cast<void>(request.command.append(std::string_view{command}));
    return request;
}

std::vector<UdsResult> uds_results_of(Worker& worker) {
    std::vector<UdsResult> results{};
    UdsResult result{};
    while (worker.take_uds_result(result)) {
        results.push_back(result);
    }
    return results;
}

std::vector<std::uint8_t> data_of(const UdsResult& result) {
    return std::vector<std::uint8_t>{result.exchange.data.begin(), result.exchange.data.end()};
}

/// Connects @p worker to @p fake and waits until it has read the supported PIDs.
bool connect_worker(Worker& worker, const elm327::sim::FakeElm327& fake, const bool read_only = false) {
    Request connect{connect_request(fake.device_path())};
    connect.read_only = read_only;
    return common::is_ok(worker.start(nullptr, nullptr)) && worker.post(connect) &&
           wait_for(worker, [](const State& state) { return state.pids_known && idle(state); });
}

/// @return true if a request of @p service (two hexadecimal digits) reached the vehicle.
bool received(const elm327::sim::FakeElm327& fake, const std::string& service) {
    bool found{false};
    for (const std::string& command : fake.commands()) {
        found = found || (command.compare(0U, 2U, service) == 0);
    }
    return found;
}

}  // namespace

TEST_CASE(worker_uds_requests) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(connect_worker(*worker, fake));

    REQUIRE(post_and_wait(*worker, uds_request("read", "22F190"), 1U));
    REQUIRE(post_and_wait(*worker, uds_request("read", "221234"), 1U));
    REQUIRE(post_and_wait(*worker, uds_request("tester present", "3E00"), 1U));
    const std::vector<UdsResult> results{uds_results_of(*worker)};
    REQUIRE(results.size() == 3U);

    // The VIN, assembled from several CAN frames.
    CHECK(results[0U].outcome == UdsResult::Outcome::kPositive);
    CHECK(test::string_of(results[0U].label.text()) == "read");
    CHECK(test::string_of(results[0U].request.text()) == "22F190");
    CHECK(results[0U].exchange.service == 0x22U);
    const std::vector<std::uint8_t> vin{data_of(results[0U])};
    REQUIRE(vin.size() == 19U);
    CHECK(vin[2U] == 0x31U);  // '1', the first character.

    // An identifier the ECU does not have.
    CHECK(results[1U].outcome == UdsResult::Outcome::kNegative);
    CHECK(results[1U].exchange.nrc == 0x31U);
    CHECK(test::string_of(results[1U].reason.text()) == "requestOutOfRange (0x31)");

    CHECK(results[2U].outcome == UdsResult::Outcome::kPositive);
    CHECK(worker->state().message.view() == "tester present: TesterPresent ok");
    CHECK(!worker->state().message_failed);
}

TEST_CASE(worker_uds_write_backs_up_first) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(connect_worker(*worker, fake));

    REQUIRE(post_and_wait(*worker, uds_write(0x0407U, {0x01U}), 3U));
    const std::vector<UdsResult> results{uds_results_of(*worker)};
    REQUIRE(results.size() == 3U);
    CHECK(test::string_of(results[0U].label.text()) == "current value");
    CHECK(data_of(results[0U]) == std::vector<std::uint8_t>({0x04U, 0x07U, 0x00U}));  // The backup.
    CHECK(test::string_of(results[1U].label.text()) == "write");
    CHECK(test::string_of(results[1U].request.text()) == "2E040701");
    CHECK(results[1U].outcome == UdsResult::Outcome::kPositive);
    CHECK(test::string_of(results[2U].label.text()) == "new value");
    CHECK(data_of(results[2U]) == std::vector<std::uint8_t>({0x04U, 0x07U, 0x01U}));  // What the ECU now holds.
}

TEST_CASE(worker_uds_write_needs_a_backup) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(connect_worker(*worker, fake));

    // DID 1234 cannot be read, so there would be nothing to restore: nothing is written.
    REQUIRE(post_and_wait(*worker, uds_write(0x1234U, {0x01U}), 2U));
    const std::vector<UdsResult> results{uds_results_of(*worker)};
    REQUIRE(results.size() == 1U);
    CHECK(test::string_of(results[0U].label.text()) == "current value");
    CHECK(results[0U].outcome == UdsResult::Outcome::kNegative);
    CHECK(worker->state().message.view() ==
          "Cannot read the current value to back it up, so nothing was written.");
    CHECK(worker->state().message_failed);
    worker->stop();
    CHECK(!received(fake, "2E"));
}

TEST_CASE(worker_uds_read_only) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(connect_worker(*worker, fake, true));

    // Writes are refused before anything is sent (the write without its backup read); reads go.
    REQUIRE(post_and_wait(*worker, uds_write(0x0407U, {0x01U}), 1U));
    REQUIRE(post_and_wait(*worker, uds_request("session", "1003"), 1U));
    REQUIRE(post_and_wait(*worker, uds_request("read", "22F190"), 1U));
    const std::vector<UdsResult> results{uds_results_of(*worker)};
    REQUIRE(results.size() == 3U);
    CHECK(test::string_of(results[0U].label.text()) == "write");
    CHECK(results[0U].outcome == UdsResult::Outcome::kRefused);
    CHECK(test::string_of(results[0U].reason.text()) == "refused in read-only mode");
    CHECK(results[1U].outcome == UdsResult::Outcome::kRefused);
    CHECK(results[2U].outcome == UdsResult::Outcome::kPositive);
    worker->stop();
    CHECK(!received(fake, "2E"));
    CHECK(!received(fake, "10"));
}

TEST_CASE(worker_raw_reports_adapter_errors) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(connect_worker(*worker, fake));

    // The addressing of expert mode is accepted silently; a command the adapter does not know is
    // reported. The first message after both is about the second, so the first told nothing.
    REQUIRE(worker->post(raw_request("ATSH7E0")));
    REQUIRE(post_and_wait(*worker, raw_request("ATXYZ"), 1U));
    CHECK(worker->state().message_failed);
    CHECK(worker->state().message.view() == "ATXYZ: ?");
    CHECK(has_line(trace_of(*worker), "ATSH7E0"));
}

namespace {

using elm327::gui::FeatureResult;

/// The built-in definitions of the simulated car.
elm327::expert::FeatureList simulator_features() {
    elm327::expert::FeatureList features{};
    elm327::expert::FeatureParseError error{};
    static_cast<void>(
        elm327::expert::parse_features(test::span_of(elm327::gui::simulator_features()), features, error));
    return features;
}

Request feature_request(const RequestKind kind, const elm327::expert::Feature& feature, const std::uint16_t index,
                        const bool on) {
    Request request{};
    request.kind = kind;
    request.feature = feature;
    request.feature_index = index;
    request.feature_on = on;
    return request;
}

/// Posts @p request and waits up to 10 s for its FeatureResult, and for the State published after it.
bool run_feature(Worker& worker, const Request& request, FeatureResult& result) {
    static_cast<void>(worker.update_state());
    const std::uint64_t before{worker.state().version};
    bool done{worker.post(request)};
    const common::TimePoint deadline{common::MonotonicClock::now() + 10s};
    bool taken{false};
    while (done && (!taken) && (common::MonotonicClock::now() < deadline)) {
        taken = worker.take_feature_result(result);
        if (!taken) {
            std::this_thread::sleep_for(2ms);
        }
    }
    // The state, with the message of a change, is published right after the result: the first idle
    // state after the request began.
    return taken && wait_for(worker, [before](const State& state) { return (state.version > before) && idle(state); });
}

}  // namespace

TEST_CASE(worker_features) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(connect_worker(*worker, fake));
    const elm327::expert::FeatureList features{simulator_features()};
    REQUIRE(features.size() == 5U);

    FeatureResult result{};
    REQUIRE(run_feature(*worker, feature_request(RequestKind::kFeatureRead, features[0U], 0U, false), result));
    CHECK(result.kind == RequestKind::kFeatureRead);
    CHECK(result.index == 0U);
    CHECK(result.reading.state == elm327::expert::FeatureState::kOff);

    REQUIRE(run_feature(*worker, feature_request(RequestKind::kFeatureChange, features[0U], 0U, true), result));
    CHECK(result.kind == RequestKind::kFeatureChange);
    CHECK(result.reading.state == elm327::expert::FeatureState::kOn);
    CHECK(result.reading.written.size() == 4U);
    CHECK(worker->state().message.view() == "Needle sweep: turned on.");
    CHECK(!worker->state().message_failed);
    CHECK(fake.did_value("714", 0x0600U) == std::vector<std::uint8_t>({0x13U, 0x00U, 0x40U, 0x00U}));

    // Undo: the window sends the value read before the change.
    Request undo{feature_request(RequestKind::kFeatureRestore, features[0U], 0U, false)};
    undo.backup = result.reading.value;
    REQUIRE(run_feature(*worker, undo, result));
    CHECK(result.kind == RequestKind::kFeatureRestore);
    CHECK(result.reading.state == elm327::expert::FeatureState::kOff);
    CHECK(worker->state().message.view() == "Needle sweep: restored.");
    CHECK(fake.did_value("714", 0x0600U) == std::vector<std::uint8_t>({0x12U, 0x00U, 0x40U, 0x00U}));
}

TEST_CASE(worker_features_read_only) {
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    const auto worker = std::make_unique<Worker>();
    REQUIRE(connect_worker(*worker, fake, true));
    const elm327::expert::FeatureList features{simulator_features()};
    REQUIRE(features.size() == 5U);

    FeatureResult result{};
    REQUIRE(run_feature(*worker, feature_request(RequestKind::kFeatureChange, features[0U], 0U, true), result));
    CHECK(result.reading.state == elm327::expert::FeatureState::kRefused);
    CHECK(result.reading.written.empty());
    CHECK(worker->state().message.view() == "Needle sweep: Read-only mode is on: nothing was changed.");
    CHECK(worker->state().message_failed);
    worker->stop();
    CHECK(!received(fake, "2E"));
    CHECK(!received(fake, "10"));
}
