#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>

#include "common/core/status.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "elm327/obd/pid.hpp"
#include "gui/gauge.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using elm327::gui::DashboardPlan;
using elm327::gui::DashboardSpace;
using elm327::gui::GaugeSize;
using elm327::gui::GaugeSpec;
using elm327::gui::ZoneKind;

namespace {

bool near(const float first, const float second) {
    return std::fabs(first - second) < 0.001F;
}

std::string label_of(const GaugeSpec& spec, const std::uint32_t index) {
    return std::string{elm327::gui::tick_label(spec, elm327::gui::tick(spec, index)).view()};
}

/// The width of the first row and the height of a plan, as the Dashboard lays it out.
struct Extent final {
    float row{0.0F};
    float height{0.0F};
};

Extent extent_of(const DashboardPlan& plan, const std::size_t large, const std::size_t medium,
                 const std::size_t displays, const DashboardSpace& space) {
    const auto rows = [&](const std::size_t count, const float width, const float height) {
        const std::size_t per_row{elm327::gui::balanced_row(count, width, space.width, space.gap)};
        return static_cast<float>((count + per_row - 1U) / per_row) * (height + space.gap);
    };
    Extent extent{};
    extent.row = (static_cast<float>(large) * plan.large) + (static_cast<float>(plan.side_columns) * plan.medium) +
                 ((static_cast<float>(large + plan.side_columns) - 1.0F) * space.gap);
    extent.height = ((large > 0U) ? (plan.large + space.gap) : 0.0F) +
                    rows(medium - plan.beside, plan.medium, plan.medium) +
                    rows(displays, space.display_width, space.display_height);
    return extent;
}

constexpr float kFont{15.0F};

DashboardSpace space_of(const float width, const float height) {
    return DashboardSpace{width, height, kFont, kFont, 13.0F * kFont, 3.4F * kFont};
}

}  // namespace

TEST_CASE(gauge_table_is_consistent) {
    std::set<std::uint8_t> pids{};
    std::size_t large{0U};
    for (const GaugeSpec& spec : elm327::gui::gauges()) {
        // Every dial shows a PID with a single value.
        const common::Result<elm327::PidInfo> info{elm327::find_pid(spec.pid)};
        REQUIRE(info.has_value());
        CHECK(info.value().formula != elm327::PidFormula::kEncoded);
        CHECK(pids.insert(spec.pid).second);  // No PID twice.
        CHECK(spec.minimum < spec.maximum);
        CHECK(!spec.title.empty());
        // The numbered ticks fall on both ends of the scale.
        const std::uint32_t count{elm327::gui::tick_count(spec)};
        CHECK(count < elm327::gui::kMaxTicks);
        CHECK(((count - 1U) % spec.minor_ticks) == 0U);
        const elm327::gui::Tick first{elm327::gui::tick(spec, 0U)};
        const elm327::gui::Tick last{elm327::gui::tick(spec, count - 1U)};
        CHECK(near(first.value, spec.minimum));
        CHECK(near(last.value, spec.maximum));
        CHECK(first.major && last.major);
        CHECK(near(first.fraction, 0.0F) && near(last.fraction, 1.0F));
        for (const elm327::gui::GaugeZone& zone : spec.zones) {
            CHECK((zone.kind == ZoneKind::kNone) || ((zone.from >= spec.minimum) && (zone.to <= spec.maximum)));
        }
        if (!spec.words[0U].empty()) {
            CHECK(last.major_index < spec.words.size());
        }
        large += (spec.size == GaugeSize::kLarge) ? 1U : 0U;
    }
    CHECK(large == 2U);
    REQUIRE(elm327::gui::find_gauge(0x0CU) != nullptr);
    CHECK(elm327::gui::find_gauge(0x0CU)->title == "RPM");
    CHECK(elm327::gui::find_gauge(0x0DU)->size == GaugeSize::kLarge);
    CHECK(elm327::gui::find_gauge(0x01U) == nullptr);  // Monitor status: bit fields.
    CHECK(elm327::gui::find_gauge(0xA6U) == nullptr);  // The odometer only counts up.
    for (const std::uint8_t pid : elm327::gui::default_dashboard()) {
        CHECK(elm327::gui::find_gauge(pid) != nullptr);
    }
}

TEST_CASE(gauge_needle) {
    const GaugeSpec& rpm{*elm327::gui::find_gauge(0x0CU)};
    CHECK(near(elm327::gui::gauge_fraction(rpm, 4000.0F), 0.5F));
    CHECK(near(elm327::gui::gauge_fraction(rpm, 2000.0F), 0.25F));
    // The needle stops at the pins.
    CHECK(near(elm327::gui::gauge_fraction(rpm, -100.0F), 0.0F));
    CHECK(near(elm327::gui::gauge_fraction(rpm, 9000.0F), 1.0F));
    CHECK(near(elm327::gui::gauge_fraction(rpm, std::numeric_limits<float>::quiet_NaN()), 0.0F));
    const GaugeSpec& intake{*elm327::gui::find_gauge(0x0FU)};
    CHECK(near(elm327::gui::gauge_fraction(intake, 30.0F), 0.5F));
    // 270 degrees through the top: the middle of the scale points straight up.
    CHECK(near(elm327::gui::gauge_angle(0.0F), elm327::gui::kGaugeStartAngle));
    CHECK(near(elm327::gui::gauge_angle(1.0F), elm327::gui::kGaugeEndAngle));
    CHECK(near(elm327::gui::gauge_angle(0.5F), 1.5F * elm327::gui::kPi));
    CHECK(near(elm327::gui::to_display(elm327::Measurement::from_units(1'726'250)), 1726.25F));
    CHECK(near(elm327::gui::to_display(elm327::Measurement::from_units(-40'000)), -40.0F));
}

TEST_CASE(gauge_ticks_and_labels) {
    const GaugeSpec& rpm{*elm327::gui::find_gauge(0x0CU)};
    CHECK(elm327::gui::tick_count(rpm) == 33U);  // 0 to 8000 rpm in steps of 250.
    CHECK(!elm327::gui::tick(rpm, 1U).major);
    CHECK(elm327::gui::tick(rpm, 4U).major);
    CHECK(near(elm327::gui::tick(rpm, 4U).value, 1000.0F));
    CHECK(label_of(rpm, 0U) == "0");
    CHECK(label_of(rpm, 4U) == "1");  // x1000.
    CHECK(label_of(rpm, 32U) == "8");

    const GaugeSpec& intake{*elm327::gui::find_gauge(0x0FU)};
    CHECK(label_of(intake, 0U) == "-20");
    CHECK(label_of(intake, 4U) == "0");

    const GaugeSpec& volts{*elm327::gui::find_gauge(0x42U)};
    CHECK(label_of(volts, 0U) == "10");
    CHECK(label_of(volts, 24U) == "16");

    const GaugeSpec& fuel{*elm327::gui::find_gauge(0x2FU)};
    CHECK(elm327::gui::tick_count(fuel) == 9U);
    CHECK(label_of(fuel, 0U) == "E");
    CHECK(label_of(fuel, 2U).empty());
    CHECK(label_of(fuel, 4U) == "\u00BD");
    CHECK(label_of(fuel, 8U) == "F");
}

TEST_CASE(gauge_zones_and_units) {
    const GaugeSpec& rpm{*elm327::gui::find_gauge(0x0CU)};
    CHECK(elm327::gui::zone_of(rpm, 7000.0F) == ZoneKind::kDanger);
    CHECK(elm327::gui::zone_of(rpm, 3000.0F) == ZoneKind::kNone);
    const GaugeSpec& coolant{*elm327::gui::find_gauge(0x05U)};
    CHECK(elm327::gui::zone_of(coolant, 45.0F) == ZoneKind::kCold);
    CHECK(elm327::gui::zone_of(coolant, 90.0F) == ZoneKind::kNone);
    CHECK(elm327::gui::zone_of(coolant, 120.0F) == ZoneKind::kDanger);
    const GaugeSpec& trim{*elm327::gui::find_gauge(0x06U)};
    CHECK(elm327::gui::zone_of(trim, -20.0F) == ZoneKind::kWarning);
    CHECK(elm327::gui::zone_of(trim, 0.0F) == ZoneKind::kNone);
    CHECK(elm327::gui::zone_of(trim, 20.0F) == ZoneKind::kWarning);

    CHECK(elm327::gui::display_unit("degC") == "\u00B0C");
    CHECK(elm327::gui::display_unit("deg") == "\u00B0");
    CHECK(elm327::gui::display_unit("rpm") == "rpm");
    CHECK(elm327::gui::display_unit("").empty());
}

TEST_CASE(gauge_default_selection) {
    const common::ByteSet supported{common::ByteSet{}
                                        .with(std::uint8_t{0x0DU})
                                        .with(std::uint8_t{0x0CU})
                                        .with(std::uint8_t{0x11U})
                                        .with(std::uint8_t{0x1FU})};  // Run time: no dial.
    const common::ByteSet all{elm327::gui::default_selection(supported, 16U)};
    CHECK(all.contains(std::uint8_t{0x0CU}) && all.contains(std::uint8_t{0x0DU}) && all.contains(std::uint8_t{0x11U}));
    CHECK(!all.contains(std::uint8_t{0x1FU}));
    CHECK(!all.contains(std::uint8_t{0x05U}));  // Not supported.
    // A limit keeps the first PIDs of the default dashboard.
    const common::ByteSet two{elm327::gui::default_selection(supported, 2U)};
    CHECK(two.contains(std::uint8_t{0x0CU}) && two.contains(std::uint8_t{0x0DU}));
    CHECK(!two.contains(std::uint8_t{0x11U}));
}

TEST_CASE(gauge_balanced_rows) {
    // 8 dials of 150 in 1260: 7 would fit, but 4 and 4 look better than 7 and 1.
    CHECK(elm327::gui::balanced_row(8U, 150.0F, 1260.0F, 15.0F) == 4U);
    CHECK(elm327::gui::balanced_row(5U, 100.0F, 1000.0F, 10.0F) == 5U);
    CHECK(elm327::gui::balanced_row(7U, 100.0F, 330.0F, 10.0F) == 3U);  // 3, 3 and 1: three rows anyway.
    CHECK(elm327::gui::balanced_row(3U, 500.0F, 100.0F, 10.0F) == 1U);  // Too wide: one per row.
    CHECK(elm327::gui::balanced_row(0U, 100.0F, 1000.0F, 10.0F) == 1U);
}

TEST_CASE(gauge_dashboard_plan) {
    // The default dashboard in a wide window: two medium dials stacked on each side of the large ones.
    const DashboardSpace wide{space_of(1260.0F, 660.0F)};
    const DashboardPlan cluster{elm327::gui::plan_dashboard(2U, 8U, 0U, wide)};
    CHECK(cluster.side_columns == 2U);
    CHECK(cluster.beside == 4U);
    CHECK(near(cluster.medium * 2.0F + wide.gap, cluster.large));  // A column is as tall as a large dial.
    const Extent fitted{extent_of(cluster, 2U, 8U, 0U, wide)};
    CHECK(fitted.row <= wide.width);
    CHECK(fitted.height <= wide.height);
    CHECK(cluster.large > (20.0F * kFont));

    // In a narrow window the large dials take the width, and everything else goes below.
    const DashboardSpace narrow{space_of(640.0F, 1400.0F)};
    const DashboardPlan tall{elm327::gui::plan_dashboard(2U, 8U, 3U, narrow)};
    CHECK(tall.side_columns == 0U);
    const Extent stacked{extent_of(tall, 2U, 8U, 3U, narrow)};
    CHECK(stacked.row <= narrow.width);
    CHECK(stacked.height <= narrow.height);

    // One medium dial on each side of a single large one; never a column without a dial.
    const DashboardPlan pair{elm327::gui::plan_dashboard(1U, 2U, 0U, wide)};
    CHECK(pair.side_columns == 2U);
    CHECK(pair.beside == 2U);
    CHECK(elm327::gui::plan_dashboard(1U, 1U, 0U, wide).side_columns == 0U);
    CHECK(elm327::gui::plan_dashboard(0U, 4U, 0U, wide).side_columns == 0U);

    // Without large dials the medium ones grow to fill the space.
    const DashboardPlan grid{elm327::gui::plan_dashboard(0U, 5U, 0U, wide)};
    CHECK(grid.medium > (15.0F * kFont));
    CHECK(extent_of(grid, 0U, 5U, 0U, wide).height <= wide.height);

    // Nothing fits a tiny window: the dials keep a readable size, and the dashboard scrolls.
    const DashboardPlan tiny{elm327::gui::plan_dashboard(2U, 8U, 0U, space_of(100.0F, 100.0F))};
    CHECK(near(tiny.large, 12.0F * kFont));
    CHECK(tiny.medium >= (8.0F * kFont));
}
