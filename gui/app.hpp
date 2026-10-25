/// @file app.hpp
/// @brief The window of elm327gui: what it shows of the Worker's results, and what a click asks the
///        Worker to do.
///
/// The window has a connection bar (device, baud rate, protocol, Connect), four tabs and a status
/// line:
/// - Live data: the supported PIDs with their latest values; selected PIDs are monitored and
///   plotted;
/// - Trouble codes: the MIL status and the stored, pending and permanent DTCs;
/// - Vehicle: the VIN and what the adapter tells about itself;
/// - Terminal: the traffic with the adapter, and a line to send AT commands or OBD requests.
///
/// App only runs on the thread that draws: it never touches the serial port, so the window stays
/// responsive while the adapter takes its time.

#ifndef ELM327_GUI_APP_HPP
#define ELM327_GUI_APP_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "common/containers/static_string.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "common/time/clock.hpp"
#include "elm327/controller/controller.hpp"
#include "gui/worker.hpp"

namespace elm327 {
namespace gui {

enum class Tab : std::uint8_t {
    kLive,
    kTroubleCodes,
    kVehicle,
    kTerminal,
};

/// @return the position of @p baud_rate in the baud-rate list of the connection bar; kNotFound for a
///         rate that the list leaves out.
[[nodiscard]] common::Result<std::int32_t> find_baud_index(common::BaudRate baud_rate) noexcept;

/// What elm327gui is told on its command line.
struct StartOptions final {
    ConnectionSettings settings{};
    /// Connect at the start.
    bool connect{false};
    /// Monitor these PIDs once connected (implies connect).
    common::ByteSet monitor{};
    common::Duration interval{std::chrono::seconds{1}};
    Tab tab{Tab::kLive};
};

class App final {
public:
    /// Values kept per plotted PID.
    static constexpr std::size_t kHistorySize{240U};
    /// Lines kept in the terminal.
    static constexpr std::size_t kLogSize{1000U};

    explicit App(Worker& worker) noexcept;

    /// Fills the connection bar from @p options and posts what they ask for.
    void start(const StartOptions& options) noexcept;

    /// Takes what the worker published and draws one frame. Call between ImGui::NewFrame() and
    /// ImGui::Render().
    void draw() noexcept;

    /// @return true while the worker is busy or monitoring: the window then redraws continuously.
    [[nodiscard]] bool active() const noexcept;

private:
    /// The plot of a monitored PID.
    struct History final {
        std::uint8_t pid{0U};
        bool used{false};
        std::array<float, kHistorySize> values{};
        std::size_t count{0U};
        std::size_t next{0U};  ///< Where the next value goes; the oldest one once count is full.
    };

    void take_results() noexcept;
    void store(const Sample& sample) noexcept;
    void log(const TraceLine& line) noexcept;

    void draw_connection() noexcept;
    void draw_live() noexcept;
    void draw_pid_table(const State& state) noexcept;
    void draw_plots() noexcept;
    void draw_trouble_codes() noexcept;
    void draw_vehicle() noexcept;
    void draw_terminal() noexcept;
    void draw_status() noexcept;

    void connect() noexcept;
    void start_monitor() noexcept;
    void send_command() noexcept;
    /// Restarts the histories for the PIDs in @p pids.
    void reset_histories(const common::ByteSet& pids) noexcept;
    /// Lists the serial devices that exist, for the combo next to the device field.
    void detect_devices() noexcept;
    [[nodiscard]] std::size_t selected_count() const noexcept;
    [[nodiscard]] ConnectionSettings settings() const noexcept;

    Worker* worker_;
    Tab tab_{Tab::kLive};
    bool select_tab_{true};  ///< Make tab_ the visible tab in the next frame.

    // The connection bar.
    std::array<char, ConnectionSettings::kMaxDeviceLength + 1U> device_{};
    std::int32_t baud_index_{0};
    std::int32_t protocol_index_{0};
    std::array<common::StaticString<64U>, 13U> protocol_labels_{};
    std::array<const char*, 13U> protocol_items_{};
    std::array<common::StaticString<ConnectionSettings::kMaxDeviceLength>, 24U> detected_{};
    std::size_t detected_count_{0U};
    common::Duration timeout_{std::chrono::seconds{5}};

    // Live data.
    std::array<PidReading, 256U> latest_{};
    std::array<bool, 256U> has_latest_{};
    std::array<bool, 256U> selected_{};
    std::array<History, kMaxMonitored> histories_{};
    std::int32_t interval_ms_{1000};
    common::ByteSet pending_monitor_{};  ///< Monitor these once the supported PIDs are known.

    // The tabs that read on their first display.
    bool dtcs_requested_{false};
    bool vin_requested_{false};

    // The terminal.
    std::array<TraceLine, kLogSize> log_{};
    std::size_t log_count_{0U};
    std::size_t log_next_{0U};
    bool scroll_log_{false};
    std::array<char, Adapter::kMaxCommandLength + 1U> command_{};
};

}  // namespace gui
}  // namespace elm327

#endif  // ELM327_GUI_APP_HPP
