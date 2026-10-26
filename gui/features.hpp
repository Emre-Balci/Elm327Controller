/// @file features.hpp
/// @brief Where elm327gui finds the definitions of the Features tab, and where it records changes.
///
/// The Features tab lists the features of two sets of definitions (see expert::parse_features()):
/// the ones built in for elm327sim's simulated car, so that the tab can be tried without a car, and
/// the user's own file, by default features.ini in the configuration directory
/// ~/.config/elm327controller (on Windows, %APPDATA%\elm327controller; on Android, the app's
/// directory on the shared storage, Android/data/<app>/files). Every change the tab makes
/// is appended to backups.txt in that directory, with the setting's value before and after, so that
/// it can be restored even after the program has ended (with the Expert tab, or by hand).

#ifndef ELM327_GUI_FEATURES_HPP
#define ELM327_GUI_FEATURES_HPP

#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/core/status.hpp"
#include "elm327/expert/feature.hpp"

namespace elm327 {
namespace gui {

/// A path of a file or a directory.
using Path = common::StaticString<512U>;

/// The separator that config_directory() puts between the parts of a path.
#if defined(_WIN32)
inline constexpr char kPathSeparator{'\\'};
#else
inline constexpr char kPathSeparator{'/'};
#endif

/// The definitions of the features of elm327sim's simulated car: its instrument cluster at 714 and
/// its body control module at 70E. They identify the simulator's modules (part numbers that start
/// with "SIM-"), so on a real car they find "not this module" or "no answer" and change nothing.
[[nodiscard]] std::string_view simulator_features() noexcept;

/// The directory of the user's definitions and backups, ~/.config/elm327controller, with the home
/// directory from the password database (not the environment). On Windows it is elm327controller in
/// the user's roaming application data, the known folder that %APPDATA% names. On Android it is the
/// directory that set_config_directory() gave.
/// @return kOk; kNotFound when the home directory is unknown; kOutOfRange when the path is too long.
[[nodiscard]] common::Status config_directory(Path& directory) noexcept;

#if defined(__ANDROID__)
/// Android has no home directory: the app gives config_directory() its files directory on the
/// shared storage (ANativeActivity::externalDataPath), where a person can put features.ini and
/// find backups.txt over USB. Call once at the start, before anything asks.
/// @return kOk; kOutOfRange when @p directory is too long (config_directory() then finds none).
common::Status set_config_directory(std::string_view directory) noexcept;
#endif

/// The definitions of the Features tab, as load_definitions() found them.
struct Definitions final {
    expert::FeatureList features{};
    /// Where they come from, e.g. "4 built in for elm327sim, 2 from <path>", or where the user's
    /// file goes when there is none yet.
    common::StaticString<256U> source{};
    /// What is wrong with the user's file, e.g. "<path>: line 3: unknown key"; empty if nothing.
    common::StaticString<256U> problem{};
    /// The configuration directory, where changes are recorded; empty when there is none.
    Path directory{};
};

/// Loads the definitions of the Features tab into @p definitions: the simulator's, built in, then
/// the user's from @p file, or from features.ini in the configuration directory when @p file is
/// nullptr. The user's file is read into a buffer of static storage: call from one thread only.
void load_definitions(const char* file, Definitions& definitions) noexcept;

/// Reads the definitions in the file @p path into @p buffer and appends their features to
/// @p features.
/// @return kOk; kNotFound when there is no such file; kOutOfRange when it is larger than @p buffer;
///         the errors of common::File; otherwise the errors of expert::parse_features(), with the
///         line and the reason in @p error.
[[nodiscard]] common::Status load_features(const char* path, common::Span<std::uint8_t> buffer,
                                           expert::FeatureList& features, expert::FeatureParseError& error) noexcept;

/// Appends a line about a change of @p feature to backups.txt in @p directory, creating the
/// directory when it is missing: the feature, the module, the setting and its value @p before and
/// @p after, e.g. "Needle sweep: module 714, DID 0600, before 12 00 40 00, after 13 00 40 00". An
/// @p undo is marked as one.
/// @return kOk; otherwise the errors of opening or writing the file.
common::Status log_change(const char* directory, const expert::Feature& feature, bool undo,
                          common::Span<const std::uint8_t> before, common::Span<const std::uint8_t> after) noexcept;

}  // namespace gui
}  // namespace elm327

#endif  // ELM327_GUI_FEATURES_HPP
