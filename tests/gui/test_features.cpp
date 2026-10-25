#if defined(_WIN32)
#include <process.h>

#include <atomic>
#else
#include <stdlib.h>
#include <sys/stat.h>
#endif

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/core/status.hpp"
#include "elm327/controller/controller.hpp"
#include "elm327/expert/feature.hpp"
#include "gui/features.hpp"
#include "sim/fake_elm327.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using elm327::expert::FeatureList;
using elm327::expert::FeatureParseError;
using elm327::expert::FeatureState;

/// A new empty directory under /tmp (on Windows, under %TEMP%), removed with everything in it at
/// the end. Its path is UTF-8, as the library takes paths.
class TempDirectory final {
public:
    TempDirectory() {
#if defined(_WIN32)
        // Like mkdtemp(): a new directory that no other test or process uses.
        static std::atomic<std::uint32_t> next{0U};
        std::error_code error{};
        const std::filesystem::path base{std::filesystem::temp_directory_path(error)};
        for (std::uint32_t attempt{0U}; (!error) && path_.empty() && (attempt < 100U); ++attempt) {
            const std::filesystem::path candidate{base / ("elm327-features-" + std::to_string(::_getpid()) +
                                                          "-" + std::to_string(next.fetch_add(1U)))};
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate.u8string();
            }
        }
#else
        std::array<char, 64U> name{"/tmp/elm327-features-XXXXXX"};
        if (::mkdtemp(name.data()) != nullptr) {
            path_ = name.data();
        }
#endif
    }
    ~TempDirectory() {
        if (!path_.empty()) {
            std::error_code ignored{};
            static_cast<void>(std::filesystem::remove_all(std::filesystem::u8path(path_), ignored));
        }
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    [[nodiscard]] const std::string& path() const {
        return path_;
    }

private:
    std::string path_{};
};

void write_file(const std::string& path, const std::string& text) {
    std::ofstream file{std::filesystem::u8path(path), std::ios::binary};
    file << text;
}

std::string read_file(const std::string& path) {
    std::ifstream file{std::filesystem::u8path(path), std::ios::binary};
    std::ostringstream text{};
    text << file.rdbuf();
    return text.str();
}

FeatureList builtin() {
    FeatureList features{};
    FeatureParseError error{};
    const common::Status status{
        elm327::expert::parse_features(test::span_of(elm327::gui::simulator_features()), features, error)};
    return common::is_ok(status) ? features : FeatureList{};
}

}  // namespace

TEST_CASE(features_builtin_definitions) {
    const FeatureList features{builtin()};
    REQUIRE(features.size() == 5U);
    CHECK(features[0U].name.view() == "Needle sweep");
    CHECK(features[0U].group.view() == "Simulator: instrument cluster");
    CHECK(features[4U].name.view() == "Five flashes for a lane change");
    // Every one names a module of the simulator, so that on a real car it changes nothing.
    for (const elm327::expert::Feature& feature : features) {
        REQUIRE(!feature.part_numbers.empty());
        CHECK(feature.part_numbers[0U].view().compare(0U, 4U, "SIM-") == 0);
        CHECK(!feature.about.empty());
    }
}

TEST_CASE(features_builtin_match_the_simulator) {
    // The built-in definitions and the simulated car must agree: each one reads a known state.
    elm327::sim::FakeElm327 fake{};
    REQUIRE(fake.valid());
    elm327::ConnectionSettings settings{};
    static_cast<void>(settings.device.assign(fake.device_path()));
    elm327::Controller controller{};
    REQUIRE(common::is_ok(controller.connect(settings)));
    elm327::expert::FeatureCoder coder{controller};
    const FeatureList features{builtin()};
    REQUIRE(features.size() == 5U);
    const std::array<FeatureState, 5U> expected{{FeatureState::kOff, FeatureState::kOn, FeatureState::kOn,
                                                 FeatureState::kOff, FeatureState::kOff}};
    for (std::size_t index{0U}; index < features.size(); ++index) {
        CHECK(coder.read(features[index]).state == expected[index]);
    }
    // And each can be changed: the fold-mirrors bit of the body control module.
    CHECK(coder.change(features[3U], true).state == FeatureState::kOn);
    CHECK(fake.did_value("70E", 0x0600U) == std::vector<std::uint8_t>({0x02U, 0x08U, 0x00U}));
}

TEST_CASE(features_load_file) {
    const TempDirectory directory{};
    REQUIRE(!directory.path().empty());
    const std::string path{directory.path() + "/features.ini"};
    write_file(path, "# Mine.\n[Comfort close]\ngroup = Body\nrequest = 70E\nreply = 778\nidentify = F187 5Q0937\n"
                     "did = 0600\nlength = 3\nbyte = 2\nbit = 5\n");
    std::array<std::uint8_t, 4096U> buffer{};
    FeatureList features{builtin()};
    FeatureParseError error{};
    REQUIRE(common::is_ok(elm327::gui::load_features(path.c_str(), buffer, features, error)));
    REQUIRE(features.size() == 6U);  // Appended to the built-in ones.
    CHECK(features[5U].name.view() == "Comfort close");
    CHECK(features[5U].mask == 0x20U);

    CHECK(elm327::gui::load_features((directory.path() + "/none.ini").c_str(), buffer, features, error) ==
          common::Status::kNotFound);
    std::array<std::uint8_t, 16U> small{};
    CHECK(elm327::gui::load_features(path.c_str(), small, features, error) == common::Status::kOutOfRange);

    write_file(path, "[Broken]\nrequest = 70E\nreply = 77\n");
    CHECK(elm327::gui::load_features(path.c_str(), buffer, features, error) == common::Status::kInvalidArgument);
    CHECK(error.line == 3U);
    CHECK(test::string_of(error.message.text()) == "invalid value for 'reply': '77'");
    CHECK(features.size() == 6U);  // Nothing of the broken file.
}

TEST_CASE(features_log_change) {
    const TempDirectory directory{};
    REQUIRE(!directory.path().empty());
    // The directories are created as needed, like the configuration directory on a first change.
    const std::string folder{directory.path() + "/config/elm327controller"};
    const FeatureList features{builtin()};
    REQUIRE(!features.empty());
    const std::vector<std::uint8_t> before{0x12U, 0x00U, 0x40U, 0x00U};
    const std::vector<std::uint8_t> after{0x13U, 0x00U, 0x40U, 0x00U};
    CHECK(common::is_ok(
        elm327::gui::log_change(folder.c_str(), features[0U], false, test::bytes_of(before), test::bytes_of(after))));
    CHECK(common::is_ok(
        elm327::gui::log_change(folder.c_str(), features[0U], true, test::bytes_of(after), test::bytes_of(before))));
    const std::string file{folder + "/backups.txt"};
    CHECK(read_file(file) ==
          "Needle sweep: module 714, DID 0600, before 12 00 40 00, after 13 00 40 00\n"
          "Needle sweep (undone): module 714, DID 0600, before 13 00 40 00, after 12 00 40 00\n");
#if !defined(_WIN32)
    struct stat status{};
    REQUIRE(::stat(file.c_str(), &status) == 0);
    CHECK((status.st_mode & 0777U) == 0600U);  // The user's own: it holds the car's settings.
#endif
}

TEST_CASE(features_config_directory) {
    elm327::gui::Path directory{};
    const common::Status status{elm327::gui::config_directory(directory)};
    // Where the account has a home directory, the definitions live in its configuration directory.
    if (common::is_ok(status)) {
        const std::string path{directory.view()};
#if defined(_WIN32)
        // In the roaming application data, which an administrator may have moved elsewhere.
        const std::string suffix{"\\elm327controller"};
#else
        const std::string suffix{"/.config/elm327controller"};
#endif
        REQUIRE(path.size() > suffix.size());
        CHECK(path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0);
    } else {
        CHECK(status == common::Status::kNotFound);
    }
}
