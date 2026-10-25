#include "elm327/expert/coding.hpp"

#include <array>
#include <cstddef>

namespace elm327 {
namespace expert {
namespace {

/// A reference map, not a database: feature, usual module, UDS operation and what to verify. The
/// identifiers and values differ by model, year and firmware, so each note says to read the current
/// value, change only the documented field and keep the original. Sources are the owner tools and
/// communities named in README.md (VCDS/OBDeleven, ddt4all, BimmerCode); no car-specific bytes are
/// shipped here.
constexpr std::array<CodingHint, 9U> kCatalog{{
    {Make::kVag, "Needle sweep / gauge staging on start",
     "Instrument cluster (VCDS module 17)", "WriteDataByIdentifier (long-coding bit)",
     "Often a single coding bit; labelled 'Staging' or 'Indicator Celebration' and placed "
     "differently across platforms. Read the cluster coding, flip the documented bit, write it "
     "back, keep the original hex to revert."},
    {Make::kVag, "'Comfort' turn signals (one-touch blink count)",
     "Body control / central electrics (VCDS module 09)", "Adaptation channel (WriteDataByIdentifier)",
     "An adaptation value (e.g. the number of flashes). Confirm the channel for your BCM; a value "
     "out of range is rejected with requestOutOfRange (NRC 31)."},
    {Make::kVag, "Seatbelt / 'lane assist' warning chime behaviour",
     "Depends on model (cluster or BCM)", "Adaptation / long coding",
     "Market- and model-specific, and disabling a safety chime may not be road-legal where you "
     "are. Verify against your own module and local rules before changing it."},
    {Make::kRenault, "One-touch indicator (lane-change blink count)",
     "Body control module / UCH", "WriteDataByIdentifier via the ECU's ddt4all definition",
     "ddt4all exposes it behind expert mode; the identifier comes from that ECU's XML. Back up the "
     "value first -- a wrong write to the UCH can disturb other body functions."},
    {Make::kRenault, "Headlight / daytime running light options",
     "Body control module / UCH", "WriteDataByIdentifier",
     "Available options vary by trim and region. Read the current configuration, change one field, "
     "and re-read to confirm."},
    {Make::kRenault, "Speed-warning / seatbelt chime options",
     "Instrument cluster or UCH", "Adaptation / WriteDataByIdentifier",
     "Safety-related and region-regulated. Verify for your model and keep the original value."},
    {Make::kBmw, "Digital speed readout in the cluster",
     "Instrument cluster (KOMBI)", "Coding (WriteDataByIdentifier)",
     "E-series and F-series code in completely different ways, so a guide for one does not carry "
     "over. Use the coding data for your exact chassis and back up the module first."},
    {Make::kBmw, "Fold mirrors on lock / unfold on unlock",
     "Body / access module (e.g. CAS or FEM/BDC by generation)", "Coding (WriteDataByIdentifier)",
     "The module and identifier depend on the generation. Read, change the one documented value, "
     "and keep the original to restore."},
    {Make::kBmw, "Auto start/stop default-off memory",
     "Body / power-management module", "Coding (WriteDataByIdentifier)",
     "Reversible, but located differently across generations; confirm against your chassis's "
     "coding data before writing."},
}};

}  // namespace

const char* make_name(const Make make) noexcept {
    const char* name{"unknown"};
    switch (make) {
        case Make::kVag:
            name = "VAG";
            break;
        case Make::kRenault:
            name = "Renault";
            break;
        case Make::kBmw:
            name = "BMW";
            break;
    }
    return name;
}

common::Span<const CodingHint> coding_catalog() noexcept {
    return common::Span<const CodingHint>{kCatalog.data(), kCatalog.size()};
}

}  // namespace expert
}  // namespace elm327
