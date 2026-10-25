#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/core/status.hpp"
#include "elm327/controller/controller.hpp"
#include "elm327/expert/feature.hpp"
#include "elm327/obd/service.hpp"
#include "sim/fake_elm327.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using elm327::expert::Feature;
using elm327::expert::FeatureCoder;
using elm327::expert::FeatureList;
using elm327::expert::FeatureParseError;
using elm327::expert::FeatureReading;
using elm327::expert::FeatureState;

/// Two modules of the simulated car: three features of its cluster and body control module.
constexpr std::string_view kDefinitions{R"(# Features of the simulated car.
[Needle sweep]
group    = Instrument cluster
about    = Sweeps the needles when the ignition comes on.
request  = 714
reply    = 77E
identify = F187 SIM-CLUSTER
did      = 0600
length   = 4
byte     = 0
bit      = 0

; The speed in figures.
[Digital speed]
group    = Instrument cluster
request  = 714
reply    = 77E
identify = F187 SIM-CLUSTER
did      = 0600
length   = 4
byte     = 2
bit      = 6

[Comfort turn signals]
group    = Body control
request  = 70E
reply    = 778
identify = F187 SIM-BCM
did      = 0410
length   = 1
byte     = 0
mask     = FF
on       = 05
off      = 03
note     = Five flashes instead of three.
)"};

FeatureList parse(const std::string_view text) {
    FeatureList features{};
    FeatureParseError error{};
    const common::Status status{elm327::expert::parse_features(test::span_of(text), features, error)};
    return common::is_ok(status) ? features : FeatureList{};
}

/// "line N: message" for definitions that must not parse.
std::string error_of(const std::string& text) {
    FeatureList features{};
    FeatureParseError error{};
    const common::Status status{elm327::expert::parse_features(test::span_of(text), features, error)};
    const std::string message{test::string_of(error.message.text())};
    return common::is_ok(status) ? std::string{"(parsed)"} : ("line " + std::to_string(error.line) + ": " + message);
}

/// A minimal complete definition, with @p extra lines after its header.
std::string definition(const std::string& extra) {
    return "[Feature]\nrequest = 714\nreply = 77E\nidentify = F187 SIM-CLUSTER\ndid = 0600\nlength = 4\nbyte = 0\n" +
           extra;
}

/// A Controller connected to a simulated adapter and car.
class Bench final {
public:
    Bench() {
        static_cast<void>(settings_.device.assign(fake_.device_path()));
        connected_ = fake_.valid() && common::is_ok(controller_.connect(settings_));
    }

    [[nodiscard]] bool connected() const {
        return connected_;
    }

    elm327::sim::FakeElm327& fake() {
        return fake_;
    }

    elm327::Controller& controller() {
        return controller_;
    }

    FeatureCoder& coder() {
        return coder_;
    }

    /// @return the index of the first command sent after @p after that starts with @p prefix; -1 if none.
    long index_of(const std::string& prefix, const long after = -1) const {
        const std::vector<std::string> commands{fake_.commands()};
        long found{-1};
        const std::size_t start{static_cast<std::size_t>(after + 1)};
        for (std::size_t index{start}; (found < 0) && (index < commands.size()); ++index) {
            if (commands[index].compare(0U, prefix.size(), prefix) == 0) {
                found = static_cast<long>(index);
            }
        }
        return found;
    }

private:
    elm327::sim::FakeElm327 fake_{};
    elm327::ConnectionSettings settings_{};
    elm327::Controller controller_{};
    FeatureCoder coder_{controller_};
    bool connected_{false};
};

std::vector<std::uint8_t> bytes_of(const elm327::expert::FeatureReading& reading) {
    return std::vector<std::uint8_t>{reading.value.begin(), reading.value.end()};
}

std::vector<std::uint8_t> written_of(const elm327::expert::FeatureReading& reading) {
    return std::vector<std::uint8_t>{reading.written.begin(), reading.written.end()};
}

}  // namespace

TEST_CASE(feature_parse_definitions) {
    const FeatureList features{parse(kDefinitions)};
    REQUIRE(features.size() == 3U);
    const Feature& sweep{features[0U]};
    CHECK(sweep.name.view() == "Needle sweep");
    CHECK(sweep.group.view() == "Instrument cluster");
    CHECK(sweep.about.view() == "Sweeps the needles when the ignition comes on.");
    CHECK(sweep.request.view() == "714");
    CHECK(sweep.reply.view() == "77E");
    CHECK(!sweep.extended);
    CHECK(sweep.session == 0x03U);  // The extended session unless the definition says otherwise.
    CHECK(sweep.identify_did == 0xF187U);
    REQUIRE(sweep.part_numbers.size() == 1U);
    CHECK(sweep.part_numbers[0U].view() == "SIM-CLUSTER");
    CHECK(sweep.did == 0x0600U);
    CHECK(sweep.length == 4U);
    CHECK(sweep.byte == 0U);
    CHECK(sweep.mask == 0x01U);  // bit 0: set when on.
    CHECK(sweep.on == 0x01U);
    CHECK(sweep.off == 0x00U);

    CHECK(features[1U].mask == 0x40U);  // bit 6.
    const Feature& signals{features[2U]};
    CHECK(signals.mask == 0xFFU);
    CHECK(signals.on == 0x05U);
    CHECK(signals.off == 0x03U);
    CHECK(signals.note.view() == "Five flashes instead of three.");
}

TEST_CASE(feature_parse_options) {
    // Several part numbers, extended addressing, another session, 29-bit identifiers, CRLF.
    const FeatureList features{parse("[Mirrors]\r\nrequest = 18DA40F1\r\nreply = 18DAF140\r\n"
                                     "identify = F187 5G0920 | 3G0920 |  8V0920\r\naddress = 60\r\nsession = 02\r\n"
                                     "did = 3000\r\nlength = 2\r\nbyte = 1\r\nmask = 0C\r\non = 04\r\noff = 08\r\n"
                                     "\r\n[Any cluster]\nrequest = 714\nreply = 77E\nidentify = F187 *\n"
                                     "did = 0600\nlength = 4\nbyte = 3\nbit = 7\n")};
    REQUIRE(features.size() == 2U);
    const Feature& mirrors{features[0U]};
    CHECK(mirrors.request.view() == "18DA40F1");
    REQUIRE(mirrors.part_numbers.size() == 3U);
    CHECK(mirrors.part_numbers[1U].view() == "3G0920");
    CHECK(mirrors.part_numbers[2U].view() == "8V0920");
    CHECK(mirrors.extended);
    CHECK(mirrors.address == 0x60U);
    CHECK(mirrors.session == 0x02U);
    CHECK(mirrors.mask == 0x0CU);
    CHECK(features[1U].part_numbers.empty());  // "*": any module at that address.
    CHECK(features[1U].mask == 0x80U);
}

TEST_CASE(feature_parse_errors) {
    CHECK(error_of("request = 714\n") == "line 1: a key before the first [feature] 'request = 714'");
    CHECK(error_of("[Unclosed\n") == "line 1: a feature's header ends with ']' '[Unclosed'");
    CHECK(error_of("[ ]\n") == "line 1: a feature needs a name '[ ]'");
    // A missing key is reported at the feature's header.
    CHECK(error_of("# Comment\n[Sweep]\nrequest = 714\n") ==
          "line 2: missing 'reply', the CAN identifier of the module's replies, in 'Sweep'");
    CHECK(error_of("[Sweep]\nrequest = 71\n") == "line 2: invalid value for 'request': '71'");
    CHECK(error_of("[Sweep]\nidentify = F187\n") == "line 2: invalid value for 'identify': 'F187'");
    CHECK(error_of(definition("bit = 8\n")) == "line 8: invalid value for 'bit': '8'");
    CHECK(error_of(definition("colour = red\n")) == "line 8: unknown key 'colour'");
    CHECK(error_of(definition("did = 0601\n")) == "line 8: given twice 'did'");
    CHECK(error_of(definition("color\n")) == "line 8: expected 'key = value' 'color'");
    CHECK(error_of(definition("")) == "line 1: missing 'bit' (or 'mask', 'on' and 'off') in 'Feature'");
    CHECK(error_of(definition("bit = 0\nmask = 01\n")) ==
          "line 1: use 'bit', or 'mask', 'on' and 'off', not both, in 'Feature'");
    CHECK(error_of(definition("mask = 01\non = 03\noff = 00\n")) ==
          "line 1: 'on' and 'off' must stay within 'mask' in 'Feature'");
    CHECK(error_of(definition("mask = 03\non = 01\noff = 01\n")) == "line 1: 'on' and 'off' must differ in 'Feature'");
    CHECK(error_of("[F]\nrequest = 714\nreply = 77E\nidentify = F187 X\ndid = 0600\nlength = 4\nbyte = 4\nbit = 0\n") ==
          "line 1: 'byte' must be less than 'length' in 'F'");
}

TEST_CASE(feature_parse_keeps_nothing_of_a_bad_text) {
    FeatureList features{parse(kDefinitions)};
    REQUIRE(features.size() == 3U);
    FeatureParseError error{};
    // One good definition, then a bad one: neither is kept.
    const std::string text{definition("bit = 0\n") + "[Bad]\nrequest = 714\n"};
    CHECK(elm327::expert::parse_features(test::span_of(text), features, error) == common::Status::kInvalidArgument);
    CHECK(features.size() == 3U);
    CHECK(error.line == 9U);

    std::string many{};
    for (std::size_t index{0U}; index <= elm327::expert::kMaxFeatures; ++index) {
        many += definition("bit = 0\n");
    }
    FeatureList full{};
    CHECK(elm327::expert::parse_features(test::span_of(many), full, error) == common::Status::kOutOfRange);
    CHECK(full.empty());
    CHECK(test::string_of(error.message.text()) == "too many features (at most 64)");
}

TEST_CASE(feature_reads_states) {
    Bench bench{};
    REQUIRE(bench.connected());
    const FeatureList features{parse(kDefinitions)};
    REQUIRE(features.size() == 3U);

    const FeatureReading sweep{bench.coder().read(features[0U])};
    CHECK(sweep.state == FeatureState::kOff);
    CHECK(bytes_of(sweep) == std::vector<std::uint8_t>({0x12U, 0x00U, 0x40U, 0x00U}));
    CHECK(sweep.written.empty());
    CHECK(bench.coder().read(features[1U]).state == FeatureState::kOn);
    CHECK(bench.coder().read(features[2U]).state == FeatureState::kOff);

    // The module was addressed with its identifiers, and the adapter reset to the OBD addressing.
    const long header{bench.index_of("ATSH714")};
    CHECK(header >= 0);
    CHECK(bench.index_of("ATCRA77E", header) > header);
    CHECK(bench.index_of("ATFCSH714", header) > header);
    CHECK(bench.index_of("ATFCSM1", header) > header);
    const long reset{bench.index_of("ATSH7DF", header)};
    CHECK(reset > bench.index_of("220600", header));
    // Reading changes nothing: no session, no write.
    CHECK(bench.index_of("10") < 0);
    CHECK(bench.index_of("2E") < 0);
}

TEST_CASE(feature_change_backs_up_and_verifies) {
    Bench bench{};
    REQUIRE(bench.connected());
    const FeatureList features{parse(kDefinitions)};
    REQUIRE(features.size() == 3U);
    const Feature& sweep{features[0U]};

    const FeatureReading on{bench.coder().change(sweep, true)};
    CHECK(on.state == FeatureState::kOn);
    CHECK(on.reason.empty());
    CHECK(bytes_of(on) == std::vector<std::uint8_t>({0x12U, 0x00U, 0x40U, 0x00U}));    // The backup.
    CHECK(written_of(on) == std::vector<std::uint8_t>({0x13U, 0x00U, 0x40U, 0x00U}));  // Only bit 0 changed.
    CHECK(bench.fake().did_value("714", 0x0600U) == std::vector<std::uint8_t>({0x13U, 0x00U, 0x40U, 0x00U}));
    // In order: identify, read, extended session, write, read again, default session, reset addressing.
    const long identify{bench.index_of("22F187")};
    const long session{bench.index_of("1003", identify)};
    const long write{bench.index_of("2E060013004000", session)};
    const long verify{bench.index_of("220600", write)};
    const long back{bench.index_of("1001", verify)};
    CHECK(identify >= 0);
    CHECK(session > identify);
    CHECK(write > session);
    CHECK(verify > write);
    CHECK(back > verify);
    CHECK(bench.index_of("ATSH7DF", back) > back);

    // Already on: nothing is written.
    const FeatureReading again{bench.coder().change(sweep, true)};
    CHECK(again.state == FeatureState::kOn);
    CHECK(test::string_of(again.reason.text()) == "It is on already.");
    CHECK(again.written.empty());
    CHECK(bench.index_of("2E", write) < 0);

    // Undo: the backup goes back.
    const FeatureReading undone{
        bench.coder().restore(sweep, common::Span<const std::uint8_t>{on.value.data(), on.value.size()})};
    CHECK(undone.state == FeatureState::kOff);
    CHECK(bench.fake().did_value("714", 0x0600U) == std::vector<std::uint8_t>({0x12U, 0x00U, 0x40U, 0x00U}));
    // A backup of another length does not fit the setting.
    const std::vector<std::uint8_t> short_backup{0x12U};
    CHECK(bench.coder().restore(sweep, test::bytes_of(short_backup)).state == FeatureState::kFailed);
}

TEST_CASE(feature_change_field) {
    Bench bench{};
    REQUIRE(bench.connected());
    const FeatureList features{parse(kDefinitions)};
    REQUIRE(features.size() == 3U);
    const Feature& signals{features[2U]};
    CHECK(bench.coder().change(signals, true).state == FeatureState::kOn);
    CHECK(bench.fake().did_value("70E", 0x0410U) == std::vector<std::uint8_t>({0x05U}));
    CHECK(bench.coder().change(signals, false).state == FeatureState::kOff);
    CHECK(bench.fake().did_value("70E", 0x0410U) == std::vector<std::uint8_t>({0x03U}));
}

TEST_CASE(feature_guards) {
    Bench bench{};
    REQUIRE(bench.connected());

    // Another module than the definition's: nothing is written.
    const FeatureList other{parse(definition("bit = 0\n") + "")};
    REQUIRE(other.size() == 1U);
    Feature wrong{other[0U]};
    wrong.part_numbers[0U].clear();
    static_cast<void>(wrong.part_numbers[0U].append("5G0920"));
    const FeatureReading not_this{bench.coder().change(wrong, true)};
    CHECK(not_this.state == FeatureState::kNotThisEcu);
    CHECK(test::string_of(not_this.reason.text()) ==
          "This module is 'SIM-CLUSTER-01'; the definition is for '5G0920'.");
    CHECK(bench.index_of("2E") < 0);
    CHECK(bench.index_of("10") < 0);

    // A setting of another length: another module.
    Feature longer{other[0U]};
    longer.length = 5U;
    const FeatureReading length{bench.coder().change(longer, true)};
    CHECK(length.state == FeatureState::kNotThisEcu);
    CHECK(test::string_of(length.reason.text()) ==
          "The setting has 4 bytes, the definition expects 5: it is for another module.");

    // No module at the address.
    Feature absent{other[0U]};
    absent.request.clear();
    static_cast<void>(absent.request.append("720"));
    const FeatureReading none{bench.coder().read(absent)};
    CHECK(none.state == FeatureState::kNoAnswer);
    CHECK(test::string_of(none.reason.text()) == "No module answers at 720: is it fitted, and the ignition on?");

    // A setting behind security access.
    Feature locked{other[0U]};
    locked.did = 0x0601U;
    locked.length = 1U;
    const FeatureReading unlock{bench.coder().change(locked, true)};
    CHECK(unlock.state == FeatureState::kNeedsUnlock);
    CHECK(bench.fake().did_value("714", 0x0601U) == std::vector<std::uint8_t>({0x00U}));
    CHECK(bench.index_of("1001", bench.index_of("2E0601")) > 0);  // The session ends anyway.
    CHECK(bench.index_of("2E") >= 0);
    CHECK(bench.index_of("2E0600") < 0);  // Nothing else was written.
}

TEST_CASE(feature_writes_only_one_frame) {
    Bench bench{};
    REQUIRE(bench.connected());
    // The VIN of the engine module: 17 bytes, more than one CAN frame carries.
    const FeatureList features{parse("[VIN bit]\nrequest = 7E0\nreply = 7E8\nidentify = F190 1D4\ndid = F190\n"
                                     "length = 17\nbyte = 0\nbit = 0\n")};
    REQUIRE(features.size() == 1U);
    const FeatureReading reading{bench.coder().change(features[0U], false)};
    CHECK(reading.state == FeatureState::kFailed);
    CHECK(test::string_of(reading.reason.text()) ==
          "The setting has 17 bytes, but a plain ELM327 sends at most 4 in one CAN frame, so it cannot be written.");
    CHECK(reading.value.size() == 17U);  // It was read, so the state is known.
    CHECK(bench.index_of("2E") < 0);
    CHECK(bench.index_of("10") < 0);
}

TEST_CASE(feature_read_only) {
    Bench bench{};
    REQUIRE(bench.connected());
    bench.controller().set_access_policy(elm327::AccessPolicy::kReadOnly);
    const FeatureList features{parse(kDefinitions)};
    REQUIRE(features.size() == 3U);
    // Reading works; changing is refused before anything is sent.
    CHECK(bench.coder().read(features[0U]).state == FeatureState::kOff);
    const FeatureReading refused{bench.coder().change(features[0U], true)};
    CHECK(refused.state == FeatureState::kRefused);
    CHECK(test::string_of(refused.reason.text()) == "Read-only mode is on: nothing was changed.");
    const FeatureReading restore{
        bench.coder().restore(features[0U], common::Span<const std::uint8_t>{refused.value.data(), 4U})};
    CHECK(restore.state == FeatureState::kOff);  // It holds that value already: nothing to write.
    CHECK(bench.index_of("10") < 0);
    CHECK(bench.index_of("2E") < 0);
    CHECK(bench.fake().did_value("714", 0x0600U) == std::vector<std::uint8_t>({0x12U, 0x00U, 0x40U, 0x00U}));
}

TEST_CASE(feature_long_identifiers_and_extended_addressing) {
    Bench bench{};
    REQUIRE(bench.connected());
    const FeatureList features{parse("[F]\nrequest = 18DA40F1\nreply = 18DAF140\naddress = 60\nidentify = F187 *\n"
                                     "did = 3000\nlength = 2\nbyte = 0\nbit = 0\n")};
    REQUIRE(features.size() == 1U);
    CHECK(bench.coder().read(features[0U]).state == FeatureState::kNoAnswer);  // No such module here.
    const long priority{bench.index_of("ATCP18")};
    CHECK(priority >= 0);
    CHECK(bench.index_of("ATSHDA40F1", priority) > priority);
    CHECK(bench.index_of("ATCRA18DAF140", priority) > priority);
    CHECK(bench.index_of("ATFCSH18DA40F1", priority) > priority);
    const long extended{bench.index_of("ATCEA60", priority)};
    CHECK(extended > priority);
    // Reset: extended addressing off, and the 29-bit functional header.
    const long off{bench.index_of("ATCEA", extended)};
    CHECK(off > extended);
    CHECK(bench.index_of("ATSHDB33F1", off) > off);
}

TEST_CASE(feature_state_names) {
    CHECK(std::string_view{elm327::expert::to_string(FeatureState::kOn)} == "on");
    CHECK(std::string_view{elm327::expert::to_string(FeatureState::kNotThisEcu)} == "not this module");
    CHECK(std::string_view{elm327::expert::to_string(FeatureState::kNeedsUnlock)} == "needs security access");
    CHECK(std::string_view{elm327::expert::to_string(FeatureState::kRefused)} == "refused (read-only)");
}
