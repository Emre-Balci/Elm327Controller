/// @file features.hpp
/// @brief Where elm327gui finds the definitions of the Features tab, and where it records changes.
///
/// The Features tab lists the features of two sets of definitions (see expert::parse_features()):
/// the ones built in for elm327sim's simulated car, so that the tab can be tried without a car, and
/// the user's own file, by default features.ini in the configuration directory
/// ~/.config/elm327controller. Every change the tab makes is appended to backups.txt in that
/// directory, with the setting's value before and after, so that it can be restored even after the
/// program has ended (with the Expert tab, or by hand).

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

/// The definitions of the features of elm327sim's simulated car: its instrument cluster at 714 and
/// its body control module at 70E. They identify the simulator's modules (part numbers that start
/// with "SIM-"), so on a real car they find "not this module" or "no answer" and change nothing.
[[nodiscard]] std::string_view simulator_features() noexcept;

/// The directory of the user's definitions and backups, ~/.config/elm327controller, with the home
/// directory from the password database (not the environment).
/// @return kOk; kNotFound when the home directory is unknown; kOutOfRange when the path is too long.
[[nodiscard]] common::Status config_directory(Path& directory) noexcept;

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
