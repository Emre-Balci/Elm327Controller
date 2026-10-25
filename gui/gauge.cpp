#include "gui/gauge.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/numeric/float.hpp"
#include "common/protocol/text.hpp"

namespace elm327 {
namespace gui {
namespace {

// Builders that keep the table below readable.

constexpr GaugeSpec dial(const std::uint8_t pid, const std::string_view title, const std::string_view caption,
                         const float minimum, const float maximum, const float major_step,
                         const std::uint32_t minor_ticks) noexcept {
    GaugeSpec spec{};
    spec.pid = pid;
    spec.title = title;
    spec.caption = caption;
    spec.minimum = minimum;
    spec.maximum = maximum;
    spec.major_step = major_step;
    spec.minor_ticks = minor_ticks;
    return spec;
}

/// @p spec with a zone from @p from to @p to, in the first free slot.
constexpr GaugeSpec zoned(GaugeSpec spec, const float from, const float to, const ZoneKind kind) noexcept {
    const std::size_t slot{(spec.zones[0U].kind == ZoneKind::kNone) ? 0U : 1U};
    spec.zones[slot] = GaugeZone{from, to, kind};
    return spec;
}

constexpr GaugeSpec large(GaugeSpec spec) noexcept {
    spec.size = GaugeSize::kLarge;
    return spec;
}

constexpr GaugeSpec divided(GaugeSpec spec, const float divisor) noexcept {
    spec.label_divisor = divisor;
    return spec;
}

constexpr GaugeSpec worded(GaugeSpec spec, const std::array<std::string_view, kMaxGaugeWords>& words) noexcept {
    spec.words = words;
    return spec;
}

/// A degree sign and C, in UTF-8 like everything ImGui draws.
constexpr std::string_view kCelsius{"\u00B0C"};

/// A percentage from 0 to 100.
constexpr GaugeSpec percent(const std::uint8_t pid, const std::string_view title) noexcept {
    return dial(pid, title, "%", 0.0F, 100.0F, 20.0F, 4U);
}

/// A fuel trim: a correction around 0, far from it in amber.
constexpr GaugeSpec trim(const std::uint8_t pid, const std::string_view title) noexcept {
    return zoned(zoned(dial(pid, title, "%", -30.0F, 30.0F, 10.0F, 2U), -30.0F, -15.0F, ZoneKind::kWarning), 15.0F,
                 30.0F, ZoneKind::kWarning);
}

/// The dials, in the order of a dashboard: the large ones, then the instruments of a classic car,
/// in pairs (in a wide window the first two medium ones stand beside the tachometer, the next two
/// beside the speedometer), then what engine diagnostics add.
constexpr std::array<GaugeSpec, 22U> kGauges{{
    large(divided(zoned(dial(0x0CU, "RPM", "x1000", 0.0F, 8000.0F, 1000.0F, 4U), 6500.0F, 8000.0F, ZoneKind::kDanger),
                  1000.0F)),
    large(dial(0x0DU, "SPEED", "km/h", 0.0F, 240.0F, 40.0F, 4U)),
    zoned(zoned(dial(0x05U, "COOLANT", kCelsius, 40.0F, 130.0F, 30.0F, 3U), 40.0F, 60.0F, ZoneKind::kCold), 115.0F,
          130.0F, ZoneKind::kDanger),
    worded(zoned(dial(0x2FU, "FUEL", "", 0.0F, 100.0F, 25.0F, 2U), 0.0F, 12.5F, ZoneKind::kDanger),
           {{"E", "", "\u00BD", "", "F"}}),
    percent(0x11U, "THROTTLE"),
    percent(0x04U, "LOAD"),
    zoned(zoned(dial(0x42U, "VOLTS", "V", 10.0F, 16.0F, 1.0F, 4U), 10.0F, 11.5F, ZoneKind::kDanger), 15.0F, 16.0F,
          ZoneKind::kDanger),
    zoned(zoned(dial(0x5CU, "OIL", kCelsius, 40.0F, 160.0F, 20.0F, 4U), 40.0F, 60.0F, ZoneKind::kCold), 140.0F, 160.0F,
          ZoneKind::kDanger),
    dial(0x0FU, "INTAKE AIR", kCelsius, -20.0F, 80.0F, 20.0F, 4U),
    dial(0x0BU, "MANIFOLD", "kPa", 0.0F, 250.0F, 50.0F, 5U),
    dial(0x10U, "AIR FLOW", "g/s", 0.0F, 250.0F, 50.0F, 5U),
    dial(0x0EU, "TIMING", "deg", -20.0F, 60.0F, 20.0F, 4U),
    zoned(dial(0x46U, "AMBIENT", kCelsius, -40.0F, 60.0F, 20.0F, 4U), -40.0F, 0.0F, ZoneKind::kCold),
    dial(0x33U, "BAROMETER", "kPa", 70.0F, 110.0F, 10.0F, 5U),
    dial(0x5EU, "FUEL RATE", "L/h", 0.0F, 50.0F, 10.0F, 2U),
    percent(0x49U, "PEDAL"),
    percent(0x45U, "REL THROTTLE"),
    trim(0x06U, "SHORT TRIM 1"),
    trim(0x07U, "LONG TRIM 1"),
    trim(0x08U, "SHORT TRIM 2"),
    trim(0x09U, "LONG TRIM 2"),
    dial(0x0AU, "FUEL PRESS", "kPa", 0.0F, 700.0F, 100.0F, 2U),
}};

template <std::size_t Size>
constexpr bool large_first(const std::array<GaugeSpec, Size>& table) noexcept {
    bool ordered{true};
    for (std::size_t index{1U}; index < table.size(); ++index) {
        ordered = ordered && ((table[index - 1U].size == GaugeSize::kLarge) || (table[index].size != GaugeSize::kLarge));
    }
    return ordered;
}

// Also catches a wrong array size: missing entries would be PID 00, medium, after the others.
static_assert(large_first(kGauges), "kGauges must list the large gauges first");
static_assert(kGauges.back().pid != 0U, "kGauges has more entries than gauges");

/// What a dashboard shows when nobody chose.
constexpr std::array<std::uint8_t, 10U> kDefaultDashboard{{
    0x0CU, 0x0DU, 0x05U, 0x2FU, 0x11U, 0x04U, 0x42U, 0x5CU, 0x0FU, 0x0BU,
}};

/// The diameters of the dials, in multiples of the font size, and how plan_dashboard() shrinks
/// them to fit: by kShrink per step, at most kFitSteps steps.
constexpr float kMinLarge{12.0F};
constexpr float kMaxLarge{28.0F};
constexpr float kMinMedium{8.0F};
constexpr float kShrink{0.96F};
constexpr std::uint32_t kFitSteps{60U};

/// Most columns of medium dials beside the large ones: two on each side.
constexpr std::size_t kMaxSideColumns{4U};

/// The height of the rows of @p count items of @p item_width x @p item_height, see balanced_row().
float rows_height(const std::size_t count, const float item_width, const float item_height,
                  const DashboardSpace& space) noexcept {
    const std::size_t per_row{balanced_row(count, item_width, space.width, space.gap)};
    const std::size_t rows{(count + per_row - 1U) / per_row};
    return static_cast<float>(rows) * (item_height + space.gap);
}

/// The diameter of the medium dials beside large ones of @p large: two of them and a gap are as
/// tall as a large one.
float medium_beside(const float large, const DashboardSpace& space) noexcept {
    return std::max((large - space.gap) * 0.5F, kMinMedium * space.font);
}

}  // namespace

common::Span<const GaugeSpec> gauges() noexcept {
    return common::Span<const GaugeSpec>{kGauges.data(), kGauges.size()};
}

const GaugeSpec* find_gauge(const std::uint8_t pid) noexcept {
    const GaugeSpec* found{nullptr};
    for (std::size_t index{0U}; (found == nullptr) && (index < kGauges.size()); ++index) {
        if (kGauges[index].pid == pid) {
            found = &kGauges[index];
        }
    }
    return found;
}

common::Span<const std::uint8_t> default_dashboard() noexcept {
    return common::Span<const std::uint8_t>{kDefaultDashboard.data(), kDefaultDashboard.size()};
}

common::ByteSet default_selection(const common::ByteSet& supported, const std::size_t limit) noexcept {
    common::ByteSet selection{};
    std::size_t count{0U};
    for (const std::uint8_t pid : kDefaultDashboard) {
        if (supported.contains(pid) && (count < limit)) {
            selection = selection.with(pid);
            ++count;
        }
    }
    return selection;
}

float to_display(const Measurement& value) noexcept {
    return static_cast<float>(value.units()) / static_cast<float>(Measurement::scale());
}

float gauge_fraction(const GaugeSpec& spec, const float value) noexcept {
    const float span{spec.maximum - spec.minimum};
    float fraction{(span > 0.0F) ? ((value - spec.minimum) / span) : 0.0F};
    if (!(fraction > 0.0F)) {
        fraction = 0.0F;  // Below the scale, or not a number.
    } else if (fraction > 1.0F) {
        fraction = 1.0F;
    } else {
        // On the scale.
    }
    return fraction;
}

float gauge_angle(const float fraction) noexcept {
    return kGaugeStartAngle + (fraction * (kGaugeEndAngle - kGaugeStartAngle));
}

std::uint32_t tick_count(const GaugeSpec& spec) noexcept {
    std::uint32_t count{1U};
    if ((spec.major_step > 0.0F) && (spec.minor_ticks > 0U)) {
        const std::uint32_t majors{common::saturating_round<std::uint32_t>((spec.maximum - spec.minimum) / spec.major_step)};
        const std::uint64_t ticks{(std::uint64_t{majors} * spec.minor_ticks) + 1U};
        count = (ticks < kMaxTicks) ? static_cast<std::uint32_t>(ticks) : kMaxTicks;
    }
    return count;
}

Tick tick(const GaugeSpec& spec, const std::uint32_t index) noexcept {
    const std::uint32_t minor{(spec.minor_ticks > 0U) ? spec.minor_ticks : 1U};
    const float step{spec.major_step / static_cast<float>(minor)};
    Tick result{};
    result.value = spec.minimum + (static_cast<float>(index) * step);
    result.fraction = gauge_fraction(spec, result.value);
    result.major = (index % minor) == 0U;
    result.major_index = index / minor;
    return result;
}

TickLabel tick_label(const GaugeSpec& spec, const Tick& tick) noexcept {
    TickLabel label{};
    if (!spec.words[0U].empty()) {
        if (tick.major_index < spec.words.size()) {
            static_cast<void>(label.append(spec.words[tick.major_index]));  // Cannot fail: words are short.
        }
    } else {
        const float divisor{(spec.label_divisor > 0.0F) ? spec.label_divisor : 1.0F};
        const std::int64_t number{common::saturating_round<std::int32_t>(tick.value / divisor)};
        const std::uint64_t magnitude{static_cast<std::uint64_t>((number < 0) ? -number : number)};
        // Cannot fail: a sign and ten digits fit.
        static_cast<void>(((number >= 0) || label.append('-')) && common::append_unsigned(label, magnitude));
    }
    return label;
}

std::string_view display_unit(const std::string_view unit) noexcept {
    std::string_view shown{unit};
    if (unit == "degC") {
        shown = kCelsius;
    } else if (unit == "deg") {
        shown = "\u00B0";
    } else {
        // Printed as it is.
    }
    return shown;
}

DashboardPlan plan_dashboard(const std::size_t large, const std::size_t medium, const std::size_t displays,
                             const DashboardSpace& space) noexcept {
    DashboardPlan best{};
    bool found{false};
    // From the most side columns to none, so that of two plans with dials of the same size the
    // more compact one wins. Every column holds at least one dial.
    for (std::size_t round{0U}; round <= (kMaxSideColumns / 2U); ++round) {
        const std::size_t columns{kMaxSideColumns - (2U * round)};
        const bool possible{(columns == 0U) || ((large > 0U) && (columns <= medium))};
        DashboardPlan plan{};
        plan.side_columns = columns;
        plan.beside = std::min(medium, 2U * columns);
        if (large > 0U) {
            // large * D + columns * (D - gap) / 2 + (large + columns - 1) * gap fits the width.
            const float dials{static_cast<float>(large) + (0.5F * static_cast<float>(columns))};
            const float gaps{(static_cast<float>(large + columns) - 1.0F) - (0.5F * static_cast<float>(columns))};
            const float fitting{(space.width - (gaps * space.gap)) / dials};
            plan.large = std::clamp(fitting, kMinLarge * space.font, kMaxLarge * space.font);
            plan.medium = medium_beside(plan.large, space);
        } else {
            // Without large dials the others may grow as large, in as many rows as fit.
            plan.medium = std::clamp(space.width, kMinMedium * space.font, kMaxLarge * space.font);
        }
        // Shrink until it all fits the height.
        const std::size_t below{medium - plan.beside};
        bool fits{false};
        for (std::uint32_t step{0U}; (!fits) && (step < kFitSteps); ++step) {
            const float needed{((large > 0U) ? (plan.large + space.gap) : 0.0F) +
                               rows_height(below, plan.medium, plan.medium, space) +
                               rows_height(displays, space.display_width, space.display_height, space)};
            fits = needed <= space.height;
            if (!fits) {
                plan.large = std::max(plan.large * kShrink, kMinLarge * space.font);
                plan.medium = (large > 0U) ? medium_beside(plan.large, space)
                                           : std::max(plan.medium * kShrink, kMinMedium * space.font);
            }
        }
        // Side columns that make the first row wider than the space are no option.
        const float row{(static_cast<float>(large) * plan.large) + (static_cast<float>(columns) * plan.medium) +
                        ((static_cast<float>(large + columns) - 1.0F) * space.gap)};
        const bool narrow_enough{(columns == 0U) || (row <= space.width)};
        const float size{(large > 0U) ? plan.large : plan.medium};
        const float best_size{(large > 0U) ? best.large : best.medium};
        if (possible && narrow_enough && ((!found) || (size > best_size))) {
            best = plan;
            found = true;
        }
    }
    return best;
}

std::size_t balanced_row(const std::size_t count, const float size, const float width, const float gap) noexcept {
    const std::size_t fit{common::saturating_cast<std::size_t>((width + gap) / (size + gap))};
    const std::size_t most{(fit > 0U) ? fit : 1U};
    const std::size_t rows{std::max<std::size_t>((count + most - 1U) / most, 1U)};
    return std::max<std::size_t>((count + rows - 1U) / rows, 1U);
}

ZoneKind zone_of(const GaugeSpec& spec, const float value) noexcept {
    ZoneKind kind{ZoneKind::kNone};
    for (std::size_t index{0U}; (kind == ZoneKind::kNone) && (index < spec.zones.size()); ++index) {
        const GaugeZone& zone{spec.zones[index]};
        if ((zone.kind != ZoneKind::kNone) && (value >= zone.from) && (value <= zone.to)) {
            kind = zone.kind;
        }
    }
    return kind;
}

}  // namespace gui
}  // namespace elm327
