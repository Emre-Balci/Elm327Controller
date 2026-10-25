/// @file dashboard.hpp
/// @brief The widget mode of elm327gui: PID values as the gauges of a classic car dashboard.
///
/// Each shown PID with a GaugeSpec gets a round dial: a chrome bezel, a dark face with white ticks
/// and numerals, colored zones, and a needle that moves smoothly to the value instead of jumping,
/// like the needle of a real instrument. A small amber display under the needle shows the exact
/// value. The tachometer and the speedometer are drawn large, side by side, and the other gauges
/// follow in rows. PIDs without a dial (e.g. the run time or the odometer) appear as digital
/// displays below the gauges.
///
/// The Dashboard only draws; App decides what is shown and keeps the readings.

#ifndef ELM327_GUI_DASHBOARD_HPP
#define ELM327_GUI_DASHBOARD_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/protocol/delimited_framer.hpp"
#include "elm327/controller/controller.hpp"

namespace elm327 {
namespace gui {

class Dashboard final {
public:
    /// Draws the PIDs in @p shown at the cursor of the current ImGui window, using the whole
    /// remaining region. @p latest holds the last reading of each PID, valid where @p has_latest
    /// is true. Call it once per frame.
    void draw(const common::ByteSet& shown, const std::array<PidReading, 256U>& latest,
              const std::array<bool, 256U>& has_latest) noexcept;

    /// @return true while a needle still moves toward its value: the window keeps redrawing then.
    [[nodiscard]] bool moving() const noexcept {
        return moving_;
    }

private:
    /// The position on its scale that each needle shows, moving toward the value.
    std::array<float, 256U> needles_{};
    bool moving_{false};
};

}  // namespace gui
}  // namespace elm327

#endif  // ELM327_GUI_DASHBOARD_HPP
