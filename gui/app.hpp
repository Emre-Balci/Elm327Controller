/// @file app.hpp
/// @brief The window of elm327gui: what it shows of the Worker's results, and what a click asks the
///        Worker to do.
///
/// The window has a connection bar (device, baud rate, protocol, Read-only, Connect), five tabs and
/// a status line:
/// - Live data: the supported PIDs with their latest values; selected PIDs are monitored and
///   plotted. In widget mode the selected PIDs are the gauges of a classic car dashboard instead
///   (see Dashboard);
/// - Trouble codes: the MIL status and the stored, pending and permanent DTCs;
/// - Vehicle: the VIN and what the adapter tells about itself;
/// - Features: the features of the car that the definitions describe (see expert::Feature), each
///   with its state and a button to turn it on or off, and Undo -- no codes. Every change is
///   confirmed, saved first, and recorded in the backup file (see features.hpp);
/// - Terminal: the traffic with the adapter, and a line to send AT commands or OBD requests;
/// - Expert: UDS/KWP requests to an ECU (target, session, data identifiers, routines, security
///   access, reset, any request), their results, and the coding reference. Every request that can
///   change the vehicle is confirmed first and disabled in read-only mode.
///
/// App only runs on the thread that draws: it never touches the serial port, so the window stays
/// responsive while the adapter takes its time.

#ifndef ELM327_GUI_APP_HPP
#define ELM327_GUI_APP_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/static_string.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "common/time/clock.hpp"
#include "elm327/controller/controller.hpp"
#include "elm327/expert/feature.hpp"
#include "gui/dashboard.hpp"
#include "gui/features.hpp"
#include "gui/worker.hpp"

namespace elm327 {
namespace gui {

enum class Tab : std::uint8_t {
    kLive,
    kTroubleCodes,
    kVehicle,
    kFeatures,
    kTerminal,
    kExpert,
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
    /// Start in widget mode: the live data as gauges.
    bool widgets{false};
    /// Start in read-only mode: refuse anything that could change the vehicle.
    bool read_only{false};
};

class App final {
public:
    /// Values kept per plotted PID.
    static constexpr std::size_t kHistorySize{240U};
    /// Lines kept in the terminal.
    static constexpr std::size_t kLogSize{1000U};
    /// Results kept in the expert tab.
    static constexpr std::size_t kUdsLogSize{100U};
    /// An input field for hexadecimal bytes separated by spaces: the most a request carries.
    static constexpr std::size_t kBytesFieldSize{(3U * expert::kMaxRequestData) + 1U};

    explicit App(Worker& worker) noexcept;

    /// Fills the connection bar from @p options and posts what they ask for.
    void start(const StartOptions& options) noexcept;

    /// Gives the Features tab its definitions. @p source says where they come from, @p problem what
    /// was wrong with the user's file (empty if nothing), and @p log_directory where the changes
    /// are recorded (empty: they are not).
    void set_features(const expert::FeatureList& features, std::string_view source, std::string_view problem,
                      std::string_view log_directory) noexcept;

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
    /// The toolbar of the live data: widget mode, monitoring, interval, and what is selected.
    void draw_live_toolbar(const State& state) noexcept;
    /// The selection of widget mode: a gauge or a digital display for each supported PID.
    void draw_shown_choice(const State& state) noexcept;
    void draw_pid_table(const State& state) noexcept;
    void draw_plots() noexcept;
    void draw_trouble_codes() noexcept;
    void draw_vehicle() noexcept;
    void draw_features() noexcept;
    /// One feature in the table of its group: name and description, state, buttons.
    void draw_feature(std::size_t index, const State& state) noexcept;
    /// Asks before a feature is turned on or off, or undone.
    void draw_feature_confirmation() noexcept;
    void draw_terminal() noexcept;
    void draw_expert() noexcept;
    /// The requests of expert mode, left of their results.
    void draw_expert_controls(const State& state) noexcept;
    void draw_expert_results() noexcept;
    void draw_coding_reference() noexcept;
    /// The confirmation of a request that can change the vehicle, while one waits for it.
    void draw_confirmation() noexcept;
    void draw_status() noexcept;
    void log_uds(const UdsResult& result) noexcept;

    // The Features tab.
    /// Checks every feature again; the requests go out as the worker's queue takes them.
    void check_features() noexcept;
    /// Posts a request of @p kind for feature @p index. @return true if the worker took it.
    bool post_feature(RequestKind kind, std::size_t index, bool on) noexcept;
    void take_feature(const FeatureResult& result) noexcept;
    /// Forgets the requests on their way, after a cancel or when the connection ends.
    void forget_feature_requests() noexcept;

    // The actions of expert mode. Each checks its fields, and says what is wrong in expert_error_.
    void apply_target() noexcept;
    void change_session() noexcept;
    void tester_present() noexcept;
    void read_did() noexcept;
    void write_did() noexcept;
    void run_routine() noexcept;
    void request_seed() noexcept;
    void send_key() noexcept;
    void reset_ecu() noexcept;
    void send_any() noexcept;
    /// Posts @p request at once when it only reads; otherwise asks first, telling @p summary.
    void submit(const Request& request, bool writes, common::Span<const char> summary) noexcept;

    void connect() noexcept;
    void start_monitor() noexcept;
    /// Turns widget mode on: selects the default dashboard if no selected PID has a gauge, and
    /// starts monitoring.
    void start_dashboard() noexcept;
    /// Selects @p pid or not, restarting a running monitor with the new selection.
    void select(std::uint8_t pid, bool selected) noexcept;
    void send_command() noexcept;
    /// Restarts the histories for the PIDs in @p pids.
    void reset_histories(const common::ByteSet& pids) noexcept;
    /// Lists the serial devices that exist, for the combo next to the device field.
    void detect_devices() noexcept;
    [[nodiscard]] std::size_t selected_count() const noexcept;
    /// The selected PIDs: monitored, plotted, and shown in widget mode.
    [[nodiscard]] common::ByteSet selection() const noexcept;
    [[nodiscard]] ConnectionSettings settings() const noexcept;

    Worker* worker_;
    Tab tab_{Tab::kLive};
    bool select_tab_{true};  ///< Make tab_ the visible tab in the next frame.

    // The connection bar.
    std::array<char, ConnectionSettings::kMaxDeviceLength + 1U> device_{};
    std::int32_t baud_index_{0};
    std::int32_t protocol_index_{0};
    bool read_only_{false};  ///< Connect in read-only mode; toggled by the connection bar.
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

    // Widget mode.
    bool widget_mode_{false};
    /// Widget mode has started monitoring for this connection, or a monitor ran: it does not start
    /// one again by itself after the user stopped it.
    bool dashboard_started_{false};
    Dashboard dashboard_{};

    // The tabs that read on their first display.
    bool dtcs_requested_{false};
    bool vin_requested_{false};

    // The terminal.
    std::array<TraceLine, kLogSize> log_{};
    std::size_t log_count_{0U};
    std::size_t log_next_{0U};
    bool scroll_log_{false};
    std::array<char, Adapter::kMaxCommandLength + 1U> command_{};

    // The Features tab: the definitions, what each feature was found to be, and a change waiting for
    // confirmation.
    struct FeatureView final {
        bool checked{false};  ///< A reading arrived since the connection began.
        bool busy{false};     ///< A request for it is on its way.
        expert::FeatureReading reading{};
        /// The setting before the first change of this connection: what Undo writes back. A backup
        /// never outlives the connection, so that it is never written into another car.
        bool has_backup{false};
        common::StaticVector<std::uint8_t, expert::kMaxFeatureValue> backup{};
    };
    expert::FeatureList features_{};
    std::array<FeatureView, expert::kMaxFeatures> feature_views_{};
    common::StaticString<256U> features_source_{};
    common::StaticString<256U> features_problem_{};
    Path log_directory_{};
    std::size_t check_next_{0U};      ///< The next feature to check; features_.size() when none is due.
    bool features_requested_{false};  ///< Checked since the connection began.
    bool hide_absent_{true};          ///< Hide the features the car does not have.
    std::size_t confirm_index_{0U};
    bool confirm_on_{false};
    bool confirm_undo_{false};
    bool open_feature_confirmation_{false};

    // Expert mode: the fields, the results, and a request waiting for confirmation.
    std::array<char, 9U> target_header_{};   ///< e.g. "7E0"; ATSH.
    std::array<char, 9U> target_reply_{};    ///< e.g. "7E8"; ATCRA, empty for any.
    std::int32_t session_index_{2};          ///< The extended session.
    std::array<char, 5U> did_{};             ///< e.g. "F190".
    std::array<char, kBytesFieldSize> value_{};
    std::int32_t routine_type_index_{0};     ///< Start.
    std::array<char, 5U> routine_{};
    std::array<char, kBytesFieldSize> routine_data_{};
    std::array<char, 3U> security_level_{};  ///< The seed level, odd; the key goes with the next one.
    std::array<char, kBytesFieldSize> security_key_{};
    std::int32_t reset_index_{2};            ///< A soft reset.
    std::array<char, Adapter::kMaxCommandLength + 1U> any_request_{};
    common::StaticString<192U> expert_error_{};
    std::array<UdsResult, kUdsLogSize> uds_log_{};
    std::size_t uds_log_count_{0U};
    std::size_t uds_log_next_{0U};
    bool scroll_uds_log_{false};
    std::int32_t coding_make_index_{0};  ///< All makes.
    bool coding_open_{false};            ///< The coding reference was open in the last frame.
    Request pending_{};
    common::StaticString<512U> pending_summary_{};
    bool open_confirmation_{false};  ///< Open the confirmation in the next frame.
    bool confirmed_{false};          ///< Its "I accept the risk" box.
};

}  // namespace gui
}  // namespace elm327

#endif  // ELM327_GUI_APP_HPP
