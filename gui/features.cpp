#include "gui/features.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/file/file.hpp"
#include "common/protocol/text.hpp"

namespace elm327 {
namespace gui {
namespace {

/// The features of the simulated car (see sim/fake_elm327.cpp, example_vehicle()).
constexpr std::string_view kSimulatorFeatures{R"(
# The features of elm327sim's simulated car, built into elm327gui so that the Features tab can be
# tried without a car. They match only the simulator, whose modules report part numbers that start
# with SIM-: on a real car they find "not this module" or "no answer", and change nothing.

[Needle sweep]
group    = Simulator: instrument cluster
about    = Sweeps the needles across the dials when the ignition comes on.
note     = Takes effect the next time the ignition comes on.
request  = 714
reply    = 77E
identify = F187 SIM-CLUSTER
did      = 0600
length   = 4
byte     = 0
bit      = 0

[Digital speed display]
group    = Simulator: instrument cluster
about    = Shows the speed in figures between the dials.
request  = 714
reply    = 77E
identify = F187 SIM-CLUSTER
did      = 0600
length   = 4
byte     = 2
bit      = 6

[Daytime running lights]
group    = Simulator: body control module
about    = Lights the daytime running lights whenever the engine runs.
request  = 70E
reply    = 778
identify = F187 SIM-BCM
did      = 0600
length   = 3
byte     = 1
bit      = 3

[Fold mirrors when locking]
group    = Simulator: body control module
about    = Folds the door mirrors in when the car is locked, and out again when it is unlocked.
request  = 70E
reply    = 778
identify = F187 SIM-BCM
did      = 0600
length   = 3
byte     = 0
bit      = 1

[Five flashes for a lane change]
group    = Simulator: body control module
about    = A short tap on the indicator stalk flashes five times instead of three.
request  = 70E
reply    = 778
identify = F187 SIM-BCM
did      = 0410
length   = 1
byte     = 0
mask     = FF
on       = 05
off      = 03
)"};

constexpr std::string_view kDirectoryName{"/.config/elm327controller"};
constexpr std::string_view kBackupFile{"/backups.txt"};

/// Permissions: the user's own (octal 0700 for a directory, 0600 for the backups).
constexpr mode_t kDirectoryMode{0x1C0U};
constexpr std::uint32_t kBackupMode{0x180U};

/// Room for the strings of a password database entry.
constexpr std::size_t kPasswordBuffer{4096U};

/// Creates @p directory and the directories above it that are missing, like "mkdir -p". Best
/// effort: opening a file in it tells whether it worked.
void make_directories(const common::Span<const char> directory) noexcept {
    Path prefix{};
    for (std::size_t index{0U}; index < directory.size(); ++index) {
        static_cast<void>(prefix.append(directory[index]));
        const bool component_end{((index + 1U) == directory.size()) || (directory[index + 1U] == '/')};
        if (component_end && (index > 0U)) {
            static_cast<void>(::mkdir(prefix.c_str(), kDirectoryMode));  // Fails when it exists: fine.
        }
    }
}

/// Appends @p bytes in hexadecimal separated by spaces, or "(none)".
template <std::size_t Capacity>
bool append_value(common::StaticString<Capacity>& text, const common::Span<const std::uint8_t> bytes) noexcept {
    return bytes.empty() ? text.append("(none)") : common::append_hex_bytes(text, bytes, ' ');
}

}  // namespace

std::string_view simulator_features() noexcept {
    return kSimulatorFeatures;
}

common::Status config_directory(Path& directory) noexcept {
    directory.clear();
    passwd entry{};
    passwd* found{nullptr};
    std::array<char, kPasswordBuffer> strings{};
    const int error{::getpwuid_r(::getuid(), &entry, strings.data(), strings.size(), &found)};
    common::Status status{((error == 0) && (found != nullptr) && (found->pw_dir != nullptr))
                              ? common::Status::kOk
                              : common::Status::kNotFound};
    if (is_ok(status)) {
        const bool fits{directory.append(std::string_view{found->pw_dir}) && directory.append(kDirectoryName)};
        status = fits ? common::Status::kOk : common::Status::kOutOfRange;
    }
    return status;
}

common::Status load_features(const char* const path, const common::Span<std::uint8_t> buffer,
                             expert::FeatureList& features, expert::FeatureParseError& error) noexcept {
    error = expert::FeatureParseError{};
    common::File file{};
    common::Status status{file.open(path, common::FileOptions{})};  // Read-only; the file must exist.
    std::size_t size{0U};
    if (is_ok(status)) {
        const common::Result<std::uint64_t> length{file.size()};
        if (!length.has_value()) {
            status = length.status();
        } else if (length.value() > buffer.size()) {
            status = common::Status::kOutOfRange;
        } else {
            size = static_cast<std::size_t>(length.value());
            status = file.read_exact(buffer.first(size));
        }
        static_cast<void>(file.close());  // Read: an error on close changes nothing.
    }
    if (is_ok(status)) {
        // char may access the bytes of any object (exception to MISRA C++:2023 Rule 8.2.5); the
        // characters are only compared, never converted to numbers (Rule 7.0.3).
        const common::Span<const char> text{reinterpret_cast<const char*>(buffer.data()), size};
        status = expert::parse_features(text, features, error);
    }
    return status;
}

common::Status log_change(const char* const directory, const expert::Feature& feature, const bool undo,
                          const common::Span<const std::uint8_t> before,
                          const common::Span<const std::uint8_t> after) noexcept {
    const std::string_view folder{directory};
    Path path{};
    common::Status status{(path.append(folder) && path.append(kBackupFile)) ? common::Status::kOk
                                                                            : common::Status::kOutOfRange};
    common::StaticString<512U> line{};
    if (is_ok(status)) {
        const bool fits{line.append(feature.name.text()) && line.append(std::string_view{undo ? " (undone)" : ""}) &&
                        line.append(": module ") && line.append(feature.request.text()) && line.append(", DID ") &&
                        common::append_hex(line, feature.did, 4U) && line.append(", before ") &&
                        append_value(line, before) && line.append(", after ") && append_value(line, after) &&
                        line.append('\n')};
        status = fits ? common::Status::kOk : common::Status::kOutOfRange;
    }
    common::File file{};
    if (is_ok(status)) {
        make_directories(common::Span<const char>{folder.data(), folder.size()});
        common::FileOptions options{};
        options.access = common::FileAccess::kWriteOnly;
        options.creation = common::FileCreation::kCreateOrOpen;
        options.append = true;
        options.permissions = kBackupMode;
        status = file.open(path.c_str(), options);
    }
    if (is_ok(status)) {
        status = file.write_all(line.bytes());
        const common::Status closed{file.close()};
        status = is_ok(status) ? closed : status;
    }
    return status;
}

}  // namespace gui
}  // namespace elm327
