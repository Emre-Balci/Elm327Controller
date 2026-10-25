#include "gui/dashboard.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "common/containers/static_string.hpp"
#include "common/containers/static_vector.hpp"
#include "common/numeric/float.hpp"
#include "elm327/obd/pid.hpp"
#include "gui/gauge.hpp"
#include "imgui.h"

namespace elm327 {
namespace gui {
namespace {

/// Most dials and displays of each kind; App shows at most kMaxMonitored PIDs.
constexpr std::size_t kMaxShown{32U};

/// How fast a needle follows its value: it covers about 1 - e^-1 of the way in 1/kNeedleSpeed s.
constexpr float kNeedleSpeed{8.0F};

/// A needle closer to its value than this has arrived.
constexpr float kNeedleRest{0.001F};

// The proportions of a dial, relative to its radius.
constexpr float kBezel{0.93F};        ///< Where the chrome bezel ends and the face begins.
constexpr float kTickOuter{0.86F};    ///< The outer end of every tick.
constexpr float kMajorInner{0.72F};   ///< The inner end of a numbered tick.
constexpr float kMinorInner{0.79F};   ///< The inner end of a small tick.
constexpr float kZoneRadius{0.835F};  ///< The middle of a colored zone, under the outer ends of the ticks.
constexpr float kNumerals{0.57F};     ///< Where the numbers stand.
constexpr float kNeedleLength{0.80F};
constexpr float kDisplayHeight{0.62F};  ///< Below the center, in the gap of the scale.
constexpr float kNumeralSize{0.16F};
constexpr float kTitleSize{0.14F};
constexpr float kTitleWidth{0.62F};  ///< Titles stay between the numerals.
constexpr float kCaptionSize{0.10F};
constexpr float kDisplaySize{0.13F};

constexpr std::size_t kBezelRings{6U};
constexpr std::size_t kFaceRings{10U};

constexpr ImVec4 kShadow{0.0F, 0.0F, 0.0F, 0.35F};
constexpr ImVec4 kChromeLight{0.82F, 0.84F, 0.87F, 1.0F};
constexpr ImVec4 kChromeDark{0.24F, 0.25F, 0.28F, 1.0F};
constexpr ImVec4 kFaceEdge{0.03F, 0.035F, 0.04F, 1.0F};
constexpr ImVec4 kFaceCenter{0.12F, 0.13F, 0.145F, 1.0F};
constexpr ImVec4 kTick{0.94F, 0.94F, 0.94F, 1.0F};
constexpr ImVec4 kMinorTick{0.68F, 0.70F, 0.73F, 1.0F};
constexpr ImVec4 kTitle{0.66F, 0.69F, 0.74F, 1.0F};
constexpr ImVec4 kCaption{0.50F, 0.53F, 0.58F, 1.0F};
constexpr ImVec4 kDanger{0.92F, 0.22F, 0.20F, 1.0F};
constexpr ImVec4 kWarning{1.0F, 0.70F, 0.0F, 1.0F};
constexpr ImVec4 kCold{0.26F, 0.62F, 0.96F, 1.0F};
constexpr ImVec4 kNeedle{1.0F, 0.34F, 0.06F, 1.0F};
constexpr ImVec4 kNeedleIdle{0.42F, 0.30F, 0.26F, 1.0F};
constexpr ImVec4 kNeedleLight{1.0F, 0.66F, 0.46F, 1.0F};
constexpr ImVec4 kNeedleShadow{0.0F, 0.0F, 0.0F, 0.45F};
constexpr ImVec4 kCap{0.15F, 0.16F, 0.18F, 1.0F};
constexpr ImVec4 kCapRing{0.56F, 0.59F, 0.63F, 1.0F};
constexpr ImVec4 kCapGlint{1.0F, 1.0F, 1.0F, 0.28F};
constexpr ImVec4 kGlint{1.0F, 1.0F, 1.0F, 0.045F};
constexpr ImVec4 kDisplay{0.05F, 0.035F, 0.01F, 1.0F};
constexpr ImVec4 kDisplayBorder{0.26F, 0.22F, 0.14F, 1.0F};
constexpr ImVec4 kDisplayText{1.0F, 0.72F, 0.26F, 1.0F};
constexpr ImVec4 kDisplayIdle{0.45F, 0.40F, 0.32F, 1.0F};
constexpr ImVec4 kPanel{0.07F, 0.075F, 0.085F, 1.0F};
constexpr ImVec4 kPanelBorder{0.22F, 0.23F, 0.26F, 1.0F};

/// The text of a value: the exact measurement with its unit, as everywhere in elm327.
using ValueText = common::StaticString<kMaxReadingText>;

ImU32 color(const ImVec4& value) noexcept {
    return ImGui::ColorConvertFloat4ToU32(value);
}

/// @return @p from at 0, @p to at 1, and the colors between.
ImVec4 mix(const ImVec4& from, const ImVec4& to, const float t) noexcept {
    return ImVec4{from.x + ((to.x - from.x) * t), from.y + ((to.y - from.y) * t), from.z + ((to.z - from.z) * t),
                  from.w + ((to.w - from.w) * t)};
}

/// @return the point at @p radius from @p center in the direction @p angle.
ImVec2 at(const ImVec2& center, const float radius, const float angle) noexcept {
    return ImVec2{center.x + (radius * std::cos(angle)), center.y + (radius * std::sin(angle))};
}

ImVec2 plus(const ImVec2& first, const ImVec2& second) noexcept {
    return ImVec2{first.x + second.x, first.y + second.y};
}

ImVec4 zone_color(const ZoneKind kind) noexcept {
    ImVec4 result{kTick};
    switch (kind) {
        case ZoneKind::kNone:
            break;
        case ZoneKind::kCold:
            result = kCold;
            break;
        case ZoneKind::kWarning:
            result = kWarning;
            break;
        case ZoneKind::kDanger:
            result = kDanger;
            break;
    }
    return result;
}

/// Draws @p text centered on @p center, at @p size or smaller so that it is at most @p width wide.
void text_at(ImDrawList& draw, ImFont* const font, const float size, const ImVec2& center, const ImU32 text_color,
             const std::string_view text, const float width = std::numeric_limits<float>::max()) noexcept {
    if (!text.empty()) {
        const char* const begin{text.data()};
        const char* const end{text.data() + text.size()};
        ImVec2 extent{font->CalcTextSizeA(size, std::numeric_limits<float>::max(), 0.0F, begin, end)};
        float fitted{size};
        if (extent.x > width) {
            // The width of a text grows with its size.
            fitted = size * (width / extent.x);
            extent = font->CalcTextSizeA(fitted, std::numeric_limits<float>::max(), 0.0F, begin, end);
        }
        draw.AddText(font, fitted, ImVec2{center.x - (extent.x * 0.5F), center.y - (extent.y * 0.5F)}, text_color,
                     begin, end);
    }
}

/// The chrome bezel and the domed face: rings from light to dark, and a face that is lighter in
/// its middle.
void draw_face(ImDrawList& draw, const ImVec2& center, const float radius) noexcept {
    draw.AddCircleFilled(ImVec2{center.x, center.y + (radius * 0.04F)}, radius * 1.02F, color(kShadow));
    for (std::size_t ring{0U}; ring < kBezelRings; ++ring) {
        const float t{static_cast<float>(ring) / static_cast<float>(kBezelRings - 1U)};
        draw.AddCircleFilled(center, radius * (1.0F - (t * (1.0F - kBezel))), color(mix(kChromeLight, kChromeDark, t)));
    }
    for (std::size_t ring{0U}; ring < kFaceRings; ++ring) {
        const float t{static_cast<float>(ring) / static_cast<float>(kFaceRings)};
        draw.AddCircleFilled(center, radius * (kBezel - 0.015F) * (1.0F - (t * 0.9F)),
                             color(mix(kFaceEdge, kFaceCenter, t)));
    }
}

/// The colored zones, the ticks and the numerals.
void draw_scale(ImDrawList& draw, ImFont* const font, const ImVec2& center, const float radius,
                const GaugeSpec& spec) noexcept {
    for (const GaugeZone& zone : spec.zones) {
        if (zone.kind != ZoneKind::kNone) {
            draw.PathArcTo(center, radius * kZoneRadius, gauge_angle(gauge_fraction(spec, zone.from)),
                           gauge_angle(gauge_fraction(spec, zone.to)));
            draw.PathStroke(color(zone_color(zone.kind)), radius * 0.05F);
        }
    }
    const std::uint32_t count{tick_count(spec)};
    for (std::uint32_t index{0U}; index < count; ++index) {
        const Tick mark{tick(spec, index)};
        const float angle{gauge_angle(mark.fraction)};
        const ZoneKind zone{zone_of(spec, mark.value)};
        // Ticks in the red zone are red; the others white.
        const ImVec4 tint{(zone == ZoneKind::kDanger) ? kDanger : (mark.major ? kTick : kMinorTick)};
        const float inner{mark.major ? kMajorInner : kMinorInner};
        draw.AddLine(at(center, radius * inner, angle), at(center, radius * kTickOuter, angle), color(tint),
                     radius * (mark.major ? 0.022F : 0.009F));
        if (mark.major) {
            const TickLabel label{tick_label(spec, mark)};
            text_at(draw, font, radius * kNumeralSize, at(center, radius * kNumerals, angle), color(tint),
                    label.view());
        }
    }
}

/// The needle at @p fraction of the scale, with its shadow and its cap.
void draw_needle(ImDrawList& draw, const ImVec2& center, const float radius, const float fraction,
                 const bool live) noexcept {
    const float angle{gauge_angle(fraction)};
    const float across{angle + (0.5F * kPi)};
    const ImVec2 tip{at(center, radius * kNeedleLength, angle)};
    const ImVec2 left{at(center, radius * 0.035F, across)};
    const ImVec2 tail{at(center, radius * 0.18F, angle + kPi)};
    const ImVec2 right{at(center, radius * 0.035F, across + kPi)};
    // Clockwise on the screen, as anti-aliased filling needs.
    const ImVec2 offset{radius * 0.015F, radius * 0.03F};
    const std::array<ImVec2, 4U> shadow{{plus(tip, offset), plus(left, offset), plus(tail, offset), plus(right, offset)}};
    draw.AddConvexPolyFilled(shadow.data(), static_cast<int>(shadow.size()), color(kNeedleShadow));
    const std::array<ImVec2, 4U> needle{{tip, left, tail, right}};
    draw.AddConvexPolyFilled(needle.data(), static_cast<int>(needle.size()), color(live ? kNeedle : kNeedleIdle));
    if (live) {
        draw.AddLine(center, tip, color(kNeedleLight), radius * 0.008F);
    }
    draw.AddCircleFilled(center, radius * 0.075F, color(kCap));
    draw.AddCircle(center, radius * 0.075F, color(kCapRing), 0, radius * 0.012F);
    draw.AddCircleFilled(at(center, radius * 0.025F, -0.75F * kPi), radius * 0.02F, color(kCapGlint));
}

/// The amber display of the exact value, centered on @p center.
void draw_display(ImDrawList& draw, ImFont* const font, const float size, const ImVec2& center,
                  const std::string_view text, const bool live) noexcept {
    const char* const begin{text.data()};
    const char* const end{text.data() + text.size()};
    const ImVec2 extent{font->CalcTextSizeA(size, std::numeric_limits<float>::max(), 0.0F, begin, end)};
    const ImVec2 padding{size * 0.45F, size * 0.18F};
    const ImVec2 top_left{center.x - (extent.x * 0.5F) - padding.x, center.y - (extent.y * 0.5F) - padding.y};
    const ImVec2 bottom_right{center.x + (extent.x * 0.5F) + padding.x, center.y + (extent.y * 0.5F) + padding.y};
    draw.AddRectFilled(top_left, bottom_right, color(kDisplay), size * 0.25F);
    draw.AddRect(top_left, bottom_right, color(kDisplayBorder), size * 0.25F, 1.0F);
    text_at(draw, font, size, center, color(live ? kDisplayText : kDisplayIdle), text);
}

/// @return the text of @p reading with its unit, or "--" without a value.
/// @return the text of @p reading with its unit as a dashboard prints it (see display_unit()), or
///         "--" without a value.
ValueText value_text(const PidReading& reading, const bool live) noexcept {
    ValueText text{};
    std::array<char, kMaxReadingText> buffer{};
    const common::Result<std::size_t> length{live ? format_reading(reading, false, ' ', buffer)
                                                  : common::Result<std::size_t>::failure(common::Status::kNotFound)};
    if (length.has_value()) {
        static_cast<void>(text.append(common::Span<const char>{buffer.data(), length.value()}));
        const common::Result<PidInfo> info{find_pid(reading.pid)};
        // A PID without a single value shows its bytes, which have no unit.
        if (reading.value.has_value() && info.has_value() && (!info.value().unit.empty())) {
            static_cast<void>(text.append(' ') && text.append(display_unit(info.value().unit)));
        }
    } else {
        static_cast<void>(text.append("--"));
    }
    return text;
}

/// Shows the name of @p pid and its value while the mouse is over the last item.
void tooltip(const std::uint8_t pid, const ValueText& value) noexcept {
    if (ImGui::IsItemHovered()) {
        const common::Result<PidInfo> info{find_pid(pid)};
        const std::string_view name{info.has_value() ? info.value().name : std::string_view{"(unknown PID)"}};
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(name.data(), name.data() + name.size());
        ImGui::TextUnformatted(value.view().data(), value.view().data() + value.size());
        ImGui::EndTooltip();
    }
}

/// Places @p count items of @p size in rows from @p top, each row centered in @p width from
/// @p left, and calls @p place(index, top-left corner) for each. @return the y below the last row.
template <typename Place>
float place_rows(const std::size_t count, const ImVec2& size, const float left, const float width, const float top,
                 const float gap, const Place& place) noexcept {
    const std::size_t per_row{balanced_row(count, size.x, width, gap)};
    float y{top};
    for (std::size_t first{0U}; first < count; first += per_row) {
        const std::size_t in_row{std::min(per_row, count - first)};
        const float row_width{(static_cast<float>(in_row) * size.x) + (static_cast<float>(in_row - 1U) * gap)};
        float x{left + std::max(0.0F, (width - row_width) * 0.5F)};
        for (std::size_t index{first}; index < (first + in_row); ++index) {
            place(index, ImVec2{x, y});
            x += size.x + gap;
        }
        y += size.y + gap;
    }
    return y;
}

}  // namespace

void Dashboard::draw(const common::ByteSet& shown, const std::array<PidReading, 256U>& latest,
                     const std::array<bool, 256U>& has_latest) noexcept {
    // The dials in the order of gauges(), large and medium apart; then the PIDs without one.
    common::StaticVector<const GaugeSpec*, kMaxShown> large{};
    common::StaticVector<const GaugeSpec*, kMaxShown> medium{};
    common::StaticVector<std::uint8_t, kMaxShown> digital{};
    for (const GaugeSpec& spec : gauges()) {
        if (shown.contains(spec.pid)) {
            // A full row drops the rest; App shows at most kMaxMonitored PIDs.
            static_cast<void>((spec.size == GaugeSize::kLarge) ? large.push_back(&spec) : medium.push_back(&spec));
        }
    }
    for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
        const std::uint8_t pid{static_cast<std::uint8_t>(value)};
        if (shown.contains(pid) && (find_gauge(pid) == nullptr) && (!is_support_pid(pid))) {
            static_cast<void>(digital.push_back(pid));
        }
    }

    const float font{ImGui::GetFontSize()};
    const float gap{font};
    const ImVec2 origin{ImGui::GetCursorScreenPos()};
    const ImVec2 region{ImGui::GetContentRegionAvail()};
    const float width{std::max(region.x, font * 10.0F)};
    const ImVec2 panel{font * 13.0F, font * 3.4F};
    const DashboardPlan plan{plan_dashboard(large.size(), medium.size(), digital.size(),
                                            DashboardSpace{width, region.y, font, gap, panel.x, panel.y})};
    const float big{plan.large};
    const float small{plan.medium};

    ImDrawList& list{*ImGui::GetWindowDrawList()};
    ImFont* const face{ImGui::GetFont()};
    const float follow{1.0F - std::exp(-ImGui::GetIO().DeltaTime * kNeedleSpeed)};
    moving_ = false;
    const auto dial = [&](const GaugeSpec& spec, const ImVec2& corner, const float diameter) noexcept {
        const PidReading& reading{latest[spec.pid]};
        const bool live{has_latest[spec.pid] && is_ok(reading.status) && reading.value.has_value()};
        const float target{live ? gauge_fraction(spec, to_display(reading.value.value())) : 0.0F};
        float& needle{needles_[spec.pid]};
        needle += (target - needle) * follow;
        moving_ = moving_ || (std::fabs(target - needle) > kNeedleRest);
        const ValueText value{value_text(reading, live)};

        ImGui::SetCursorScreenPos(corner);
        ImGui::PushID(static_cast<int>(spec.pid));
        static_cast<void>(ImGui::InvisibleButton("##dial", ImVec2{diameter, diameter}));
        tooltip(spec.pid, value);
        ImGui::PopID();

        const float radius{diameter * 0.49F};
        const ImVec2 center{corner.x + (diameter * 0.5F), corner.y + (diameter * 0.5F)};
        draw_face(list, center, radius);
        draw_scale(list, face, center, radius, spec);
        text_at(list, face, radius * kTitleSize, ImVec2{center.x, center.y - (radius * 0.30F)}, color(kTitle),
                spec.title, radius * kTitleWidth);
        text_at(list, face, radius * kCaptionSize, ImVec2{center.x, center.y - (radius * 0.165F)}, color(kCaption),
                spec.caption, radius * kTitleWidth);
        draw_display(list, face, radius * kDisplaySize, ImVec2{center.x, center.y + (radius * kDisplayHeight)},
                     value.view(), live);
        draw_needle(list, center, radius, needle, live);
        // The glass over it all.
        list.AddEllipseFilled(ImVec2{center.x, center.y - (radius * 0.45F)}, ImVec2{radius * 0.55F, radius * 0.27F},
                              color(kGlint));
    };

    // The first row: the large dials in the middle, the side columns of medium dials beside them.
    float bottom{origin.y};
    std::size_t placed{0U};
    if (!large.empty()) {
        const std::size_t columns{plan.side_columns};
        const float row{(static_cast<float>(large.size()) * big) + (static_cast<float>(columns) * small) +
                        ((static_cast<float>(large.size() + columns) - 1.0F) * gap)};
        float x{origin.x + std::max(0.0F, (width - row) * 0.5F)};
        // The dials beside spread evenly over the columns, the left ones first.
        const std::size_t base{(columns > 0U) ? (plan.beside / columns) : 0U};
        const std::size_t extra{(columns > 0U) ? (plan.beside % columns) : 0U};
        const auto column = [&](const std::size_t index) noexcept {
            const std::size_t count{base + ((index < extra) ? 1U : 0U)};
            // One dial alone stands in the middle of the column.
            const float top{origin.y + ((count == 1U) ? ((big - small) * 0.5F) : 0.0F)};
            for (std::size_t slot{0U}; (slot < count) && (placed < plan.beside); ++slot) {
                dial(*medium[placed], ImVec2{x, top + (static_cast<float>(slot) * (small + gap))}, small);
                ++placed;
            }
            x += small + gap;
        };
        for (std::size_t index{0U}; index < (columns / 2U); ++index) {
            column(index);
        }
        for (const GaugeSpec* const spec : large) {
            dial(*spec, ImVec2{x, origin.y}, big);
            x += big + gap;
        }
        for (std::size_t index{columns / 2U}; index < columns; ++index) {
            column(index);
        }
        bottom = origin.y + big + gap;
    }
    // The other medium dials in rows below.
    bottom = place_rows(medium.size() - placed, ImVec2{small, small}, origin.x, width, bottom, gap,
                        [&](const std::size_t index, const ImVec2& corner) noexcept {
                            dial(*medium[placed + index], corner, small);
                        });
    // The PIDs without a dial: digital displays with their name and value.
    bottom = place_rows(digital.size(), panel, origin.x, width, bottom, gap,
                        [&](const std::size_t index, const ImVec2& corner) noexcept {
                            const std::uint8_t pid{digital[index]};
                            const PidReading& reading{latest[pid]};
                            const bool live{has_latest[pid] && is_ok(reading.status)};
                            const ValueText value{value_text(reading, live)};
                            ImGui::SetCursorScreenPos(corner);
                            ImGui::PushID(static_cast<int>(pid));
                            static_cast<void>(ImGui::InvisibleButton("##display", panel));
                            tooltip(pid, value);
                            ImGui::PopID();
                            const ImVec2 far{corner.x + panel.x, corner.y + panel.y};
                            list.AddRectFilled(corner, far, color(kPanel), font * 0.3F);
                            list.AddRect(corner, far, color(kPanelBorder), font * 0.3F, 1.0F);
                            const common::Result<PidInfo> info{find_pid(pid)};
                            const std::string_view name{info.has_value() ? info.value().name
                                                                         : std::string_view{"(unknown PID)"}};
                            list.PushClipRect(corner, far, true);
                            list.AddText(face, font * 0.85F, ImVec2{corner.x + (font * 0.5F), corner.y + (font * 0.3F)},
                                         color(kTitle), name.data(), name.data() + name.size());
                            list.AddText(face, font * 1.3F, ImVec2{corner.x + (font * 0.5F), corner.y + (font * 1.5F)},
                                         color(live ? kDisplayText : kDisplayIdle), value.view().data(),
                                         value.view().data() + value.size());
                            list.PopClipRect();
                        });
    // Make the region scroll over everything drawn.
    ImGui::SetCursorScreenPos(ImVec2{origin.x, bottom});
    ImGui::Dummy(ImVec2{width, 0.0F});
}

}  // namespace gui
}  // namespace elm327
