/// @file gauge.hpp
/// @brief Dial gauges for PID values, in the style of a classic car dashboard: which PIDs suit a
///        dial, the scale of each, and the geometry of its ticks and needle.
///
/// A PID suits a dial when its value is one number that moves within a known range: the engine
/// speed, the vehicle speed, temperatures, pressures, percentages. Each GaugeSpec gives the range a
/// dashboard shows (narrower than the PID's coding range: a tachometer ends at 8000 rpm, not at
/// 16383.75), the numbered and the small ticks, and the zones drawn in color, e.g. the red zone of
/// the tachometer or the cold and hot ends of the coolant gauge.
///
/// Nothing here draws: the Dashboard does, with ImGui. The positions are floating point for the
/// drawing only; the values stay exact Measurements everywhere else.

#ifndef ELM327_GUI_GAUGE_HPP
#define ELM327_GUI_GAUGE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "elm327/obd/pid.hpp"

namespace elm327 {
namespace gui {

enum class GaugeSize : std::uint8_t {
    kLarge,   ///< The tachometer and the speedometer.
    kMedium,  ///< Every other gauge.
};

/// What a colored part of a scale means.
enum class ZoneKind : std::uint8_t {
    kNone,
    kCold,     ///< Blue: e.g. an engine that has not warmed up.
    kWarning,  ///< Amber: e.g. a fuel trim far from 0.
    kDanger,   ///< Red: e.g. above the maximum engine speed, an overheating engine, an empty tank.
};

/// A colored part of a scale, from one value to another.
struct GaugeZone final {
    float from{0.0F};
    float to{0.0F};
    ZoneKind kind{ZoneKind::kNone};
};

/// Most words of a scale that shows words instead of numbers, e.g. E, 1/2 and F.
inline constexpr std::size_t kMaxGaugeWords{5U};

/// The dial of one PID.
struct GaugeSpec final {
    std::uint8_t pid{0U};
    std::string_view title{};    ///< On the dial, e.g. "RPM".
    std::string_view caption{};  ///< Under the title, e.g. "x1000"; may be empty.
    float minimum{0.0F};
    float maximum{1.0F};
    float major_step{1.0F};         ///< Between the numbered ticks.
    std::uint32_t minor_ticks{1U};  ///< Steps between two numbered ticks: 1 for no small ticks.
    float label_divisor{1.0F};      ///< The numbers are value / label_divisor, e.g. 1000 for rpm.
    std::array<GaugeZone, 2U> zones{};
    /// Words at the numbered ticks instead of numbers, e.g. "E", "", "1/2", "", "F" (empty: no
    /// text at that tick). Used when the first word is not empty.
    std::array<std::string_view, kMaxGaugeWords> words{};
    GaugeSize size{GaugeSize::kMedium};
};

/// The dial sweeps 270 degrees through the top, from the bottom left (minimum) to the bottom right
/// (maximum). Angles are in radians, clockwise on the screen from the positive x axis.
inline constexpr float kPi{3.14159265F};
inline constexpr float kGaugeStartAngle{0.75F * kPi};
inline constexpr float kGaugeEndAngle{2.25F * kPi};

/// Most ticks of a scale; the specs stay well below.
inline constexpr std::uint32_t kMaxTicks{64U};

/// Every gauge, in the order a dashboard shows them: the large ones first.
[[nodiscard]] common::Span<const GaugeSpec> gauges() noexcept;

/// @return the gauge of @p pid; nullptr for a PID without one.
[[nodiscard]] const GaugeSpec* find_gauge(std::uint8_t pid) noexcept;

/// The PIDs a dashboard shows when nobody chose: the tachometer, the speedometer, the coolant
/// temperature, the throttle, the engine load, the fuel level, the voltage, ...
[[nodiscard]] common::Span<const std::uint8_t> default_dashboard() noexcept;

/// The PIDs of default_dashboard() that @p supported holds, at most @p limit of them.
[[nodiscard]] common::ByteSet default_selection(const common::ByteSet& supported, std::size_t limit) noexcept;

/// @return @p value converted for drawing, e.g. 1726.25 for 1726.25 rpm.
[[nodiscard]] float to_display(const Measurement& value) noexcept;

/// @return where @p value lies on the scale of @p spec: 0 at the minimum, 1 at the maximum.
///         Values outside the scale stop at its ends, like a needle at its pin.
[[nodiscard]] float gauge_fraction(const GaugeSpec& spec, float value) noexcept;

/// @return the angle of the needle at @p fraction of the scale, see kGaugeStartAngle.
[[nodiscard]] float gauge_angle(float fraction) noexcept;

/// A tick of a scale.
struct Tick final {
    float value{0.0F};
    float fraction{0.0F};
    bool major{false};             ///< A numbered tick.
    std::uint32_t major_index{0U};  ///< The number of the numbered tick: 0 at the minimum.
};

/// @return the number of ticks of @p spec, both ends included, at most kMaxTicks.
[[nodiscard]] std::uint32_t tick_count(const GaugeSpec& spec) noexcept;

/// @return the tick @p index of @p spec, 0 at the minimum. @pre index < tick_count(spec)
[[nodiscard]] Tick tick(const GaugeSpec& spec, std::uint32_t index) noexcept;

/// Text of a numbered tick, e.g. "40", "-20", "8" (for 8000 rpm), "1/2".
using TickLabel = common::StaticString<16U>;

/// @return the text at the numbered tick @p tick: its word, or its value / label_divisor.
[[nodiscard]] TickLabel tick_label(const GaugeSpec& spec, const Tick& tick) noexcept;

/// @return the unit @p unit of a PID (see PidInfo::unit) as a dashboard prints it: a degree sign and
///         "C" for "degC", a degree sign for "deg", and the others as they are.
[[nodiscard]] std::string_view display_unit(std::string_view unit) noexcept;

/// @return the kind of the first zone of @p spec that holds @p value; kNone if none does.
[[nodiscard]] ZoneKind zone_of(const GaugeSpec& spec, float value) noexcept;

/// The space a dashboard has, and the size of a digital display, in pixels.
struct DashboardSpace final {
    float width{0.0F};
    float height{0.0F};
    float font{1.0F};  ///< The font size: the dials stay between multiples of it.
    float gap{0.0F};   ///< Between two dials.
    float display_width{0.0F};
    float display_height{0.0F};
};

/// How a dashboard arranges its dials, like the instrument cluster of a car: the large dials side
/// by side in the middle of the first row, with columns of two medium dials beside them, one above
/// the other, as tall together as a large dial. The medium dials that do not fit beside them
/// follow in rows below, then the digital displays.
struct DashboardPlan final {
    float large{0.0F};             ///< Diameter of the large dials.
    float medium{0.0F};            ///< Diameter of the other dials.
    std::size_t side_columns{0U};  ///< Columns beside the large dials, half of them on each side.
    std::size_t beside{0U};        ///< Medium dials in those columns, the first ones of the list.
};

/// @return the plan with the largest dials that fit @p space for @p large large dials, @p medium
///         medium ones and @p displays digital displays. When nothing fits, the dials keep a
///         readable size and the dashboard needs scrolling.
[[nodiscard]] DashboardPlan plan_dashboard(std::size_t large, std::size_t medium, std::size_t displays,
                                           const DashboardSpace& space) noexcept;

/// @return how many of @p count items @p size wide go in each row of @p width with @p gap between
///         them: as few rows as possible, filled evenly, e.g. 4 and 4 rather than 7 and 1.
[[nodiscard]] std::size_t balanced_row(std::size_t count, float size, float width, float gap) noexcept;

}  // namespace gui
}  // namespace elm327

#endif  // ELM327_GUI_GAUGE_HPP
