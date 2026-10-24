#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "common/core/status.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "common/time/deadline.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/obd/client.hpp"
#include "elm327/obd/dtc.hpp"
#include "elm327/obd/vin.hpp"
#include "sim/fake_elm327.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

/// An initialized adapter on a simulated one, and a client that uses it.
class Bench final {
public:
    explicit Bench(elm327::sim::Vehicle vehicle = elm327::sim::example_vehicle()) : fake_{std::move(vehicle)} {}

    /// @return true when the adapter is open and initialized with @p protocol.
    bool start(const char protocol = '0') {
        return fake_.valid() &&
               common::is_ok(adapter_.open(fake_.device_path().c_str(), elm327::AdapterOptions{})) &&
               common::is_ok(adapter_.initialize(protocol));
    }

    elm327::sim::FakeElm327& fake() {
        return fake_;
    }

    elm327::ObdClient& obd() {
        return obd_;
    }

private:
    elm327::sim::FakeElm327 fake_;
    elm327::Adapter adapter_{};
    elm327::ObdClient obd_{adapter_};
};

std::vector<elm327::Dtc> read_dtcs(elm327::ObdClient& obd, const elm327::DtcKind kind) {
    elm327::DtcList dtcs{};
    const common::Status status{obd.read_dtcs(kind, dtcs)};
    return common::is_ok(status) ? std::vector<elm327::Dtc>{dtcs.begin(), dtcs.end()} : std::vector<elm327::Dtc>{};
}

std::size_t count_requests(const std::vector<std::string>& commands, const std::string& prefix) {
    std::size_t count{0U};
    for (const std::string& command : commands) {
        count += (command.compare(0U, prefix.size(), prefix) == 0) ? 1U : 0U;
    }
    return count;
}

}  // namespace

TEST_CASE(client_supported_pids) {
    Bench bench{};
    REQUIRE(bench.start());
    // connect() reads the first range only.
    REQUIRE(common::is_ok(bench.obd().connect()));
    const common::ByteSet& supported{bench.obd().supported_pids()};
    CHECK(supported.contains(std::uint8_t{0x00U}));
    CHECK(supported.contains(std::uint8_t{0x0CU}));
    CHECK(supported.contains(std::uint8_t{0x20U}));
    CHECK(!supported.contains(std::uint8_t{0x0AU}));
    CHECK(!supported.contains(std::uint8_t{0x21U}));

    REQUIRE(common::is_ok(bench.obd().read_supported_pids()));
    const std::initializer_list<std::uint8_t> expected{0x00U, 0x01U, 0x0CU, 0x21U, 0x42U, 0x5CU,
                                                       0xA6U, 0x20U, 0x40U, 0x60U, 0x80U, 0xA0U};
    for (const std::uint8_t pid : expected) {
        CHECK(supported.contains(pid));
    }
    CHECK(!supported.contains(std::uint8_t{0x0AU}));
    CHECK(!supported.contains(std::uint8_t{0xC0U}));
    // connect() asked for range 00, read_supported_pids() for 00, 20, 40, 60, 80 and A0.
    CHECK(count_requests(bench.fake().commands(), "01") == 7U);
}

TEST_CASE(client_read_pid) {
    Bench bench{};
    REQUIRE(bench.start());
    REQUIRE(common::is_ok(bench.obd().connect()));
    const common::Result<common::Span<const std::uint8_t>> engine_speed{bench.obd().read_pid(0x0CU)};
    REQUIRE(engine_speed.has_value());
    CHECK(test::vector_of(engine_speed.value()) == (std::vector<std::uint8_t>{0x1AU, 0xF8U}));

    // A PID the vehicle does not support: the adapter replies "NO DATA".
    CHECK(bench.obd().read_pid(0x0AU).status() == common::Status::kNotFound);
    CHECK(bench.obd().last_reply() == elm327::Reply::kNoData);
    CHECK(test::string_of(bench.obd().response().reply_line()) == "NO DATA");

    // The value follows the vehicle.
    bench.fake().set_pid(0x0CU, {0x0BU, 0xB8U});
    const common::Result<common::Span<const std::uint8_t>> idle{bench.obd().read_pid(0x0CU)};
    REQUIRE(idle.has_value());
    CHECK(test::vector_of(idle.value()) == (std::vector<std::uint8_t>{0x0BU, 0xB8U}));
}

TEST_CASE(client_dtcs_and_vin_on_can) {
    elm327::sim::Vehicle vehicle{elm327::sim::example_vehicle()};
    // Five codes take a multi-frame message on CAN.
    vehicle.stored_dtcs = {0x0133U, 0x0420U, 0x0171U, 0x0172U, 0xC073U};
    Bench bench{vehicle};
    REQUIRE(bench.start());
    REQUIRE(common::is_ok(bench.obd().connect()));
    CHECK(read_dtcs(bench.obd(), elm327::DtcKind::kStored) ==
          (std::vector<elm327::Dtc>{0x0133U, 0x0420U, 0x0171U, 0x0172U, 0xC073U}));
    CHECK(read_dtcs(bench.obd(), elm327::DtcKind::kPending) == (std::vector<elm327::Dtc>{0x0171U}));
    CHECK(read_dtcs(bench.obd(), elm327::DtcKind::kPermanent) == (std::vector<elm327::Dtc>{0x0420U}));

    elm327::Vin vin{};
    REQUIRE(common::is_ok(bench.obd().read_vin(vin)));
    CHECK(test::string_of(vin.text()) == "1D4GP00R55B123456");
}

TEST_CASE(client_legacy_vehicle) {
    elm327::sim::Vehicle vehicle{elm327::sim::example_vehicle()};
    vehicle.can = false;
    vehicle.stored_dtcs = {0x0133U, 0x0420U, 0x0171U, 0x0172U};
    vehicle.pending_dtcs = {};
    Bench bench{vehicle};
    REQUIRE(bench.start());
    REQUIRE(common::is_ok(bench.obd().read_supported_pids()));
    CHECK(bench.obd().supported_pids().contains(std::uint8_t{0xA6U}));
    // Two messages of three codes each, the last padded.
    CHECK(read_dtcs(bench.obd(), elm327::DtcKind::kStored) ==
          (std::vector<elm327::Dtc>{0x0133U, 0x0420U, 0x0171U, 0x0172U}));
    elm327::DtcList dtcs{};
    CHECK(common::is_ok(bench.obd().read_dtcs(elm327::DtcKind::kPending, dtcs)));
    CHECK(dtcs.empty());

    elm327::Vin vin{};
    REQUIRE(common::is_ok(bench.obd().read_vin(vin)));
    CHECK(test::string_of(vin.text()) == "1D4GP00R55B123456");
}

TEST_CASE(client_ignition_off) {
    elm327::sim::Vehicle vehicle{elm327::sim::example_vehicle()};
    vehicle.ignition_on = false;
    Bench bench{vehicle};
    REQUIRE(bench.start());
    CHECK(bench.obd().connect() == common::Status::kDisconnected);
    CHECK(bench.obd().last_reply() == elm327::Reply::kUnableToConnect);
    REQUIRE(bench.obd().response().size() == 2U);
    CHECK(test::string_of(bench.obd().response().line(0U)) == "SEARCHING...");
    CHECK(bench.obd().messages().empty());
}

TEST_CASE(client_wrong_protocol) {
    // Protocol 3 (ISO 9141-2) on a CAN vehicle: the bus does not wake up.
    Bench bench{};
    REQUIRE(bench.start('3'));
    CHECK(bench.obd().connect() == common::Status::kSystemError);
    CHECK(bench.obd().last_reply() == elm327::Reply::kBusError);
    CHECK(test::string_of(bench.obd().response().reply_line()) == "BUS INIT: ...ERROR");
}

TEST_CASE(client_request_errors) {
    Bench bench{};
    REQUIRE(bench.start());
    // "OK" or "?" is not a reply to an OBD request.
    CHECK(bench.obd().request("ATI0", common::Deadline::after(std::chrono::seconds{5})) ==
          common::Status::kNotSupported);
    CHECK(bench.obd().request("ATE0", common::Deadline::after(std::chrono::seconds{5})) ==
          common::Status::kInvalidArgument);
    // Text that is not OBD data.
    CHECK(bench.obd().request("ATRV", common::Deadline::after(std::chrono::seconds{5})) ==
          common::Status::kInvalidArgument);
    CHECK(common::is_ok(bench.obd().request("010C", common::Deadline::after(std::chrono::seconds{5}))));
    CHECK(bench.obd().messages().size() == 1U);
}
