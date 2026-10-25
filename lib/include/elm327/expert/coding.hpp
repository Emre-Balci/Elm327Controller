/// @file coding.hpp
/// @brief A reference map of common owner codings for VAG, Renault and BMW -- what, not how-exactly.
///
/// "Unlocking a feature" on these cars is almost always a coding or adaptation change: reading an
/// identifier (or long-coding block) from a control module, changing a documented bit or value, and
/// writing it back -- a WriteDataByIdentifier (UDS service 2E), an InputOutputControl, or a routine.
/// The mechanism is the UDS/KWP layer in uds.hpp. What differs between cars is the data: which
/// module, which identifier, which bit, and what value. That data lives in the manufacturer's own
/// databases (VCDS/OBDeleven label files for VAG, ddt4all's ECU XML for Renault, the BMW coding
/// data behind BimmerCode) and in each owner community, and it varies by model, year and firmware.
///
/// So this catalog does not ship byte values for any car: a wrong identifier or value can disable a
/// feature or upset a module. It names the feature, the module it usually lives in, and the UDS
/// operation involved, as a starting point to look up against your own vehicle. The safe routine is
/// always the same and the tooling follows it: read the current value first and keep it, change only
/// the one documented field, write it back, and read again to confirm -- so you can always restore
/// the original. Coding is done with the ignition on and a steady battery, and some of it is not
/// road-legal in every market; that is the owner's responsibility.

#ifndef ELM327_EXPERT_CODING_HPP
#define ELM327_EXPERT_CODING_HPP

#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"

namespace elm327 {
namespace expert {

/// A vehicle make this catalog has hints for.
enum class Make : std::uint8_t {
    kVag,      ///< Volkswagen, Audi, SEAT, Škoda (the VAG group).
    kRenault,  ///< Renault, Dacia (and related Nissan platforms).
    kBmw,      ///< BMW and MINI.
};

/// One feature and where to look for it. The fields are names and guidance, never car-specific byte
/// values; see the file comment.
struct CodingHint final {
    Make make;
    /// The feature in plain words, e.g. "Needle sweep (gauge staging) on start".
    std::string_view feature;
    /// The module it usually lives in, with the identifier other tools give it where it is stable,
    /// e.g. "Instrument cluster (VCDS module 17)".
    std::string_view module;
    /// The UDS/KWP operation involved, e.g. "WriteDataByIdentifier (long coding bit)".
    std::string_view method;
    /// What to verify and watch out for on your own car.
    std::string_view note;
};

/// @return the name of @p make, e.g. "VAG", "Renault" or "BMW".
[[nodiscard]] const char* make_name(Make make) noexcept;

/// All the hints, in make order.
[[nodiscard]] common::Span<const CodingHint> coding_catalog() noexcept;

}  // namespace expert
}  // namespace elm327

#endif  // ELM327_EXPERT_CODING_HPP
