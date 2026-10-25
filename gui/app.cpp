#include "gui/app.hpp"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "common/protocol/text.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/expert/coding.hpp"
#include "elm327/expert/uds.hpp"
#include "elm327/obd/dtc.hpp"
#include "elm327/obd/pid.hpp"
#include "elm327/obd/service.hpp"
#include "gui/gauge.hpp"
#include "imgui.h"

namespace elm327 {
namespace gui {
namespace {

constexpr std::array<common::BaudRate, 11U> kBaudRates{{
    common::BaudRate::k9600,
    common::BaudRate::k19200,
    common::BaudRate::k38400,
    common::BaudRate::k57600,
    common::BaudRate::k115200,
    common::BaudRate::k230400,
    common::BaudRate::k460800,
    common::BaudRate::k500000,
    common::BaudRate::k921600,
    common::BaudRate::k1000000,
    common::BaudRate::k2000000,
}};
constexpr std::array<const char*, kBaudRates.size()> kBaudNames{{
    "9600", "19200", "38400", "57600", "115200", "230400", "460800", "500000", "921600", "1000000", "2000000",
}};

/// The serial devices of USB adapters (FTDI, CH340 and PL2303 chips; CDC ACM) and of Bluetooth
/// adapters bound with rfcomm, each with the numbers 0 to 7.
constexpr std::array<std::string_view, 3U> kDevicePrefixes{{"/dev/ttyUSB", "/dev/ttyACM", "/dev/rfcomm"}};
constexpr std::string_view kDeviceNumbers{"01234567"};

/// The interval of a monitor, in ms.
constexpr std::int32_t kMinInterval{50};
constexpr std::int32_t kMaxInterval{10'000};

/// The systems of the DTCs, indexed by their two most significant bits.
constexpr std::array<const char*, 4U> kDtcSystems{{"Powertrain", "Chassis", "Body", "Network"}};
constexpr std::uint32_t kDtcSystemShift{14U};

constexpr std::array<const char*, 3U> kDtcLabels{{"Stored", "Pending", "Permanent"}};

/// The characters of the spinner of the status line.
constexpr std::string_view kSpinner{"|/-\\"};

constexpr ImVec4 kGreen{0.40F, 0.80F, 0.45F, 1.0F};
constexpr ImVec4 kRed{0.95F, 0.42F, 0.38F, 1.0F};
constexpr ImVec4 kYellow{0.95F, 0.80F, 0.38F, 1.0F};
constexpr ImVec4 kBlue{0.50F, 0.72F, 1.0F, 1.0F};

/// Text built for a widget.
using Text = common::StaticString<256U>;

void show(const common::Span<const char> text) noexcept {
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void show(const std::string_view text) noexcept {
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void show(const ImVec4& color, const common::Span<const char> text) noexcept {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    show(text);
    ImGui::PopStyleColor();
}

void show(const ImVec4& color, const std::string_view text) noexcept {
    show(color, common::Span<const char>{text.data(), text.size()});
}

void show_disabled(const std::string_view text) noexcept {
    ImGui::BeginDisabled();
    show(text);
    ImGui::EndDisabled();
}

/// A label in front of a widget on the same line.
void label(const std::string_view text) noexcept {
    ImGui::AlignTextToFramePadding();
    show(text);
    ImGui::SameLine();
}

/// The text of @p reading: its measurement, with the unit if @p with_unit, or its bytes.
Text value_text(const PidReading& reading, const bool with_unit) noexcept {
    std::array<char, kMaxReadingText> buffer{};
    const common::Result<std::size_t> length{format_reading(reading, with_unit, ' ', buffer)};
    Text text{};
    static_cast<void>(text.append(common::Span<const char>{buffer.data(), length.value_or(0U)}));
    return text;
}

Text hex_text(const std::uint64_t value, const std::size_t digits) noexcept {
    Text text{};
    static_cast<void>(common::append_hex(text, value, digits));
    return text;
}

/// The text in the NUL-terminated @p buffer of an input field.
template <std::size_t Size>
std::string_view view_of(const std::array<char, Size>& buffer) noexcept {
    std::size_t length{0U};
    while ((length < Size) && (buffer[length] != '\0')) {
        ++length;
    }
    return std::string_view{buffer.data(), length};
}

/// Copies @p text into the input field @p buffer, NUL-terminated, cut to fit.
template <std::size_t Size>
void fill(std::array<char, Size>& buffer, const common::Span<const char> text) noexcept {
    const std::size_t length{(text.size() < Size) ? text.size() : (Size - 1U)};
    for (std::size_t index{0U}; index < length; ++index) {
        buffer[index] = text[index];
    }
    buffer[length] = '\0';
}

bool is_busy(const State& state) noexcept {
    return (state.activity != Activity::kIdle) && (state.activity != Activity::kMonitoring);
}

std::int32_t milliseconds(const common::Duration duration) noexcept {
    return static_cast<std::int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

// Expert mode.

/// Text of a reply or of a confirmation: up to 128 bytes in hexadecimal, and more.
using LongText = common::StaticString<640U>;

/// Bytes typed into a field: at most a whole command's worth.
using HexBytes = common::StaticVector<std::uint8_t, Adapter::kMaxCommandLength / 2U>;

/// Fields for one hexadecimal number, and for bytes separated by spaces.
constexpr ImGuiInputTextFlags kHexFlags{ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase};
constexpr ImGuiInputTextFlags kBytesFlags{ImGuiInputTextFlags_CharsUppercase};

constexpr std::array<const char*, 4U> kSessionNames{
    {"Default (01)", "Programming (02)", "Extended (03)", "Safety system (04)"}};
constexpr std::array<expert::DiagnosticSession, 4U> kSessions{
    {expert::DiagnosticSession::kDefault, expert::DiagnosticSession::kProgramming,
     expert::DiagnosticSession::kExtended, expert::DiagnosticSession::kSafetySystem}};
constexpr std::array<const char*, 3U> kRoutineNames{{"Start (01)", "Stop (02)", "Results (03)"}};
constexpr std::array<expert::RoutineControlType, 3U> kRoutineTypes{
    {expert::RoutineControlType::kStart, expert::RoutineControlType::kStop,
     expert::RoutineControlType::kRequestResults}};
constexpr std::array<const char*, 3U> kResetNames{{"Hard (01)", "Key off/on (02)", "Soft (03)"}};
constexpr std::array<expert::EcuReset, 3U> kResets{
    {expert::EcuReset::kHardReset, expert::EcuReset::kKeyOffOnReset, expert::EcuReset::kSoftReset}};
constexpr std::array<const char*, 4U> kMakeNames{{"All makes", "VAG", "Renault", "BMW"}};
constexpr std::array<expert::Make, 3U> kMakes{{expert::Make::kVag, expert::Make::kRenault, expert::Make::kBmw}};

/// The highest seed level: its key goes with the next level, which must stay below 0x7F.
constexpr std::uint16_t kMaxSeedLevel{0x7DU};

/// The entry @p index of @p items, as chosen in a combo; the first one for an index out of range.
template <typename T, std::size_t Size>
T item_of(const std::array<T, Size>& items, const std::int32_t index) noexcept {
    const std::size_t position{static_cast<std::size_t>(index)};
    return (position < Size) ? items[position] : items[0U];
}

/// Shows @p text as the tooltip of the last widget, even when it is disabled.
void tooltip(const std::string_view text) noexcept {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0F);
        show(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// Shows @p text wrapped at the edge of the window, or of the table column.
void show_wrapped(const ImVec4& color, const common::Span<const char> text) noexcept {
    ImGui::PushTextWrapPos(0.0F);
    show(color, text);
    ImGui::PopTextWrapPos();
}

void show_wrapped(const ImVec4& color, const std::string_view text) noexcept {
    show_wrapped(color, common::Span<const char>{text.data(), text.size()});
}

void show_wrapped(const std::string_view text) noexcept {
    show_wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), text);
}

void show_note(const std::string_view text) noexcept {
    show_wrapped(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), text);
}

/// Parses one to @p digits hexadecimal digits, e.g. the DID "F190".
common::Result<std::uint16_t> parse_number(const std::string_view text, const std::size_t digits) noexcept {
    return ((!text.empty()) && (text.size() <= digits))
               ? common::parse_hex<std::uint16_t>(common::Span<const char>{text.data(), text.size()})
               : common::Result<std::uint16_t>::failure(common::Status::kInvalidArgument);
}

/// Decodes the hexadecimal bytes of @p text, e.g. "0A 1B" or "0A1B"; none for an empty text.
common::Result<HexBytes> parse_bytes(const std::string_view text) noexcept {
    std::array<std::uint8_t, Adapter::kMaxCommandLength / 2U> buffer{};
    const common::Result<std::size_t> count{common::decode_hex_bytes(
        common::Span<const char>{text.data(), text.size()}, common::Span<std::uint8_t>{buffer.data(), buffer.size()})};
    HexBytes bytes{};
    for (std::size_t index{0U}; index < count.value_or(0U); ++index) {
        static_cast<void>(bytes.push_back(buffer[index]));  // Cannot fail: the buffer holds no more.
    }
    return count.has_value() ? common::Result<HexBytes>::success(bytes)
                             : common::Result<HexBytes>::failure(count.status());
}

/// A request of the expert tab for the worker: the UDS request @p command, for what @p label says.
Request make_uds_request(const std::string_view label, const std::string_view command) noexcept {
    Request request{};
    request.kind = RequestKind::kUds;
    static_cast<void>(request.label.append(label));      // Cannot fail: the labels here are short.
    static_cast<void>(request.command.append(command));  // Cannot fail: a built command fits.
    return request;
}

/// Replaces @p text with @p message, e.g. what is wrong with a field.
void say(common::StaticString<192U>& text, const std::string_view message) noexcept {
    text.clear();
    static_cast<void>(text.append(message));
}

const char* outcome_text(const UdsResult::Outcome outcome) noexcept {
    const char* text{"failed"};
    switch (outcome) {
        case UdsResult::Outcome::kPositive:
            text = "ok";
            break;
        case UdsResult::Outcome::kNegative:
            text = "rejected";
            break;
        case UdsResult::Outcome::kRefused:
            text = "refused";
            break;
        case UdsResult::Outcome::kFailed:
            text = "failed";
            break;
    }
    return text;
}

ImVec4 outcome_color(const UdsResult::Outcome outcome) noexcept {
    ImVec4 color{kRed};
    if (outcome == UdsResult::Outcome::kPositive) {
        color = kGreen;
    } else if (outcome == UdsResult::Outcome::kRefused) {
        color = kYellow;
    } else {
        // Rejected or failed.
    }
    return color;
}

/// The data of a positive reply in hexadecimal, followed by the value of a data identifier as text
/// when it is printable, e.g. a VIN.
LongText reply_text(const expert::UdsExchange& exchange) noexcept {
    LongText text{};
    const common::Span<const std::uint8_t> data{exchange.data.data(), exchange.data.size()};
    static_cast<void>(common::append_hex_bytes(text, data, ' '));
    if ((exchange.service == expert::service_id(expert::UdsService::kReadDataByIdentifier)) && (data.size() > 2U)) {
        std::array<char, expert::kMaxReplyData> printable{};
        const common::Result<std::size_t> length{expert::format_printable(data.subspan(2U), printable)};
        if (length.has_value()) {
            static_cast<void>(text.append("   \"") &&
                              text.append(common::Span<const char>{printable.data(), length.value()}) &&
                              text.append("\""));
        }
    }
    if (exchange.truncated) {
        static_cast<void>(text.append(" ..."));
    }
    return text;
}

}  // namespace

common::Result<std::int32_t> find_baud_index(const common::BaudRate baud_rate) noexcept {
    common::Result<std::int32_t> result{common::Result<std::int32_t>::failure(common::Status::kNotFound)};
    for (std::size_t index{0U}; index < kBaudRates.size(); ++index) {
        if (kBaudRates[index] == baud_rate) {
            result = common::Result<std::int32_t>::success(static_cast<std::int32_t>(index));
        }
    }
    return result;
}

App::App(Worker& worker) noexcept : worker_{&worker} {
    const std::string_view numbers{protocols()};
    for (std::size_t index{0U}; index < protocol_labels_.size(); ++index) {
        static_cast<void>(protocol_labels_[index].append(numbers[index]) && protocol_labels_[index].append("  ") &&
                          protocol_labels_[index].append(std::string_view{protocol_name(numbers[index])}));
        protocol_items_[index] = protocol_labels_[index].c_str();
    }
    baud_index_ = find_baud_index(common::BaudRate::k38400).value_or(0);
    fill(device_, common::Span<const char>{kDefaultDevice.data(), kDefaultDevice.size()});
    // Expert mode starts with a harmless read ready: the VIN (DID F190), and the first seed level.
    fill(did_, common::Span<const char>{"F190", 4U});
    fill(security_level_, common::Span<const char>{"01", 2U});
}

void App::start(const StartOptions& options) noexcept {
    fill(device_, options.settings.device.text());
    baud_index_ = find_baud_index(options.settings.baud_rate).value_or(baud_index_);
    const std::size_t protocol{protocols().find(options.settings.protocol)};
    protocol_index_ = (protocol != std::string_view::npos) ? static_cast<std::int32_t>(protocol) : 0;
    timeout_ = options.settings.timeouts.command;
    const std::int32_t interval{milliseconds(options.interval)};
    interval_ms_ = (interval < kMinInterval) ? kMinInterval : ((interval > kMaxInterval) ? kMaxInterval : interval);
    tab_ = options.tab;
    select_tab_ = true;
    widget_mode_ = options.widgets;
    read_only_ = options.read_only;
    pending_monitor_ = options.monitor;
    for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
        selected_[value] = options.monitor.contains(static_cast<std::uint8_t>(value));
    }
    if (options.connect || (!options.monitor.empty())) {
        connect();
    }
}

bool App::active() const noexcept {
    const State& state{worker_->state()};
    return (state.activity != Activity::kIdle) || state.monitoring || (widget_mode_ && dashboard_.moving());
}

void App::draw() noexcept {
    take_results();
    const ImGuiViewport* const viewport{ImGui::GetMainViewport()};
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags flags{ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus};
    if (ImGui::Begin("elm327gui", nullptr, flags)) {
        draw_connection();
        ImGui::Separator();
        const float status_height{ImGui::GetFrameHeightWithSpacing()};
        if (ImGui::BeginChild("content", ImVec2{0.0F, -status_height})) {
            if (ImGui::BeginTabBar("tabs")) {
                const auto tab_flags = [this](const Tab tab) noexcept {
                    return (select_tab_ && (tab_ == tab)) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                };
                if (ImGui::BeginTabItem("Live data", nullptr, tab_flags(Tab::kLive))) {
                    draw_live();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Trouble codes", nullptr, tab_flags(Tab::kTroubleCodes))) {
                    draw_trouble_codes();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Vehicle", nullptr, tab_flags(Tab::kVehicle))) {
                    draw_vehicle();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Terminal", nullptr, tab_flags(Tab::kTerminal))) {
                    draw_terminal();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Expert", nullptr, tab_flags(Tab::kExpert))) {
                    draw_expert();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            select_tab_ = false;
        }
        ImGui::EndChild();
        draw_status();
    }
    ImGui::End();
}

void App::take_results() noexcept {
    static_cast<void>(worker_->update_state());
    const State& state{worker_->state()};
    if (!state.connected) {
        // The next connection reads the codes and the VIN again when their tabs open, and widget
        // mode starts its monitor again.
        dtcs_requested_ = false;
        vin_requested_ = false;
        dashboard_started_ = false;
    }
    if (state.monitoring) {
        dashboard_started_ = true;
    }
    if ((!pending_monitor_.empty()) && state.connected && state.pids_known) {
        start_monitor();
        pending_monitor_ = common::ByteSet{};
        dashboard_started_ = true;
    }
    if (widget_mode_ && (!dashboard_started_) && state.connected && state.pids_known && (!is_busy(state))) {
        start_dashboard();
    }
    // At most what the queues hold, so a frame never takes long.
    Sample sample{};
    for (std::size_t count{0U}; (count < Worker::kSampleCapacity) && worker_->take_sample(sample); ++count) {
        store(sample);
    }
    TraceLine line{};
    for (std::size_t count{0U}; (count < Worker::kTraceCapacity) && worker_->take_trace(line); ++count) {
        log(line);
    }
    UdsResult result{};
    for (std::size_t count{0U}; (count < Worker::kUdsCapacity) && worker_->take_uds_result(result); ++count) {
        log_uds(result);
    }
}

void App::store(const Sample& sample) noexcept {
    for (const PidReading& reading : sample.readings) {
        latest_[reading.pid] = reading;
        has_latest_[reading.pid] = true;
        if (sample.monitored && reading.value.has_value()) {
            for (History& history : histories_) {
                if (history.used && (history.pid == reading.pid)) {
                    // Only the plot uses floating point: the values stay exact Measurements.
                    history.values[history.next] = static_cast<float>(reading.value.value().units()) /
                                                   static_cast<float>(Measurement::scale());
                    history.next = (history.next + 1U) % kHistorySize;
                    history.count = (history.count < kHistorySize) ? (history.count + 1U) : kHistorySize;
                }
            }
        }
    }
}

void App::log(const TraceLine& line) noexcept {
    log_[log_next_] = line;
    log_next_ = (log_next_ + 1U) % kLogSize;
    log_count_ = (log_count_ < kLogSize) ? (log_count_ + 1U) : kLogSize;
}

void App::log_uds(const UdsResult& result) noexcept {
    uds_log_[uds_log_next_] = result;
    uds_log_next_ = (uds_log_next_ + 1U) % kUdsLogSize;
    uds_log_count_ = (uds_log_count_ < kUdsLogSize) ? (uds_log_count_ + 1U) : kUdsLogSize;
    scroll_uds_log_ = true;
}

void App::draw_connection() noexcept {
    const State& state{worker_->state()};
    const bool busy{is_busy(state)};
    const float font{ImGui::GetFontSize()};
    ImGui::BeginDisabled(state.connected || busy);
    label("Device");
    ImGui::SetNextItemWidth(font * 13.0F);
    if (ImGui::InputText("##device", device_.data(), device_.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        connect();
    }
    ImGui::SameLine(0.0F, 0.0F);
    if (ImGui::BeginCombo("##detected", nullptr, ImGuiComboFlags_NoPreview)) {
        if (ImGui::IsWindowAppearing()) {
            detect_devices();
        }
        for (std::size_t index{0U}; index < detected_count_; ++index) {
            if (ImGui::Selectable(detected_[index].c_str())) {
                fill(device_, detected_[index].text());
            }
        }
        if (detected_count_ == 0U) {
            show_disabled("No USB or Bluetooth serial device found.");
            show_disabled("Type the device, e.g. the one elm327sim prints.");
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    label("Baud");
    ImGui::SetNextItemWidth(font * 6.0F);
    static_cast<void>(ImGui::Combo("##baud", &baud_index_, kBaudNames.data(), static_cast<int>(kBaudNames.size())));
    ImGui::SameLine();
    label("Protocol");
    ImGui::SetNextItemWidth(font * 17.0F);
    static_cast<void>(ImGui::Combo("##protocol", &protocol_index_, protocol_items_.data(),
                                   static_cast<int>(protocol_items_.size())));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox("Read-only", &read_only_)) {
        if (state.connected) {
            connect();  // Reconnect so the new access policy takes effect at once.
        }
    }
    tooltip("Safe mode: refuse anything that could change the vehicle. Changing it while connected reconnects.");
    ImGui::SameLine();
    if (state.connected) {
        if (ImGui::Button("Disconnect")) {
            worker_->cancel();
            static_cast<void>(worker_->post(RequestKind::kDisconnect));
        }
    } else {
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Connect")) {
            connect();
        }
        ImGui::EndDisabled();
    }
    if (busy) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            worker_->cancel();
        }
    }

    if (state.connected) {
        show(kGreen, "Connected");
        ImGui::SameLine();
        Text text{};
        static_cast<void>(text.append("to ") && text.append(state.identity.text()) && text.append(" on ") &&
                          text.append(state.settings.device.text()));
        if (!state.voltage.empty()) {
            static_cast<void>(text.append(", ") && text.append(state.voltage.text()));
        }
        if (!state.protocol.empty()) {
            static_cast<void>(text.append(", ") && text.append(state.protocol.text()));
        }
        show(text.text());
    } else {
        show_disabled((state.activity == Activity::kConnecting) ? "Connecting..." : "Not connected");
    }
}

void App::draw_live() noexcept {
    const State& state{worker_->state()};
    const bool busy{is_busy(state)};
    if (!state.connected) {
        // Widget mode can be chosen before connecting: it starts once the supported PIDs are known.
        static_cast<void>(ImGui::Checkbox("Widget mode", &widget_mode_));
        show_disabled("Connect to the adapter to read live data.");
    } else if (!state.pids_known) {
        static_cast<void>(ImGui::Checkbox("Widget mode", &widget_mode_));
        show("The PIDs the vehicle supports are not known yet.");
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Read supported PIDs")) {
            static_cast<void>(worker_->post(RequestKind::kReadPids));
        }
        ImGui::EndDisabled();
    } else {
        draw_live_toolbar(state);
        if (!widget_mode_) {
            const float width{ImGui::GetContentRegionAvail().x};
            if (ImGui::BeginChild("pid_list", ImVec2{width * 0.55F, 0.0F}, ImGuiChildFlags_ResizeX)) {
                draw_pid_table(state);
            }
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("plots", ImVec2{0.0F, 0.0F})) {
                draw_plots();
            }
            ImGui::EndChild();
        } else if (selected_count() == 0U) {
            show_disabled("The vehicle supports none of the gauges of the default dashboard: choose what to show.");
        } else {
            if (ImGui::BeginChild("dashboard", ImVec2{0.0F, 0.0F})) {
                dashboard_.draw(selection(), latest_, has_latest_);
            }
            ImGui::EndChild();
        }
    }
}

void App::draw_live_toolbar(const State& state) noexcept {
    const bool busy{is_busy(state)};
    if (ImGui::Checkbox("Widget mode", &widget_mode_) && widget_mode_) {
        start_dashboard();
    }
    ImGui::SameLine();
    if (!widget_mode_) {
        ImGui::BeginDisabled(busy || state.monitoring);
        if (ImGui::Button("Read all values")) {
            Request request{};
            request.kind = RequestKind::kReadValues;
            request.pids = state.supported_pids;
            static_cast<void>(worker_->post(request));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (state.monitoring) {
        if (ImGui::Button("Stop monitoring")) {
            static_cast<void>(worker_->post(RequestKind::kStopMonitor));
        }
    } else {
        ImGui::BeginDisabled(busy || (selected_count() == 0U));
        if (ImGui::Button(widget_mode_ ? "Start monitoring" : "Monitor selected")) {
            start_monitor();
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    label("Interval");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0F);
    static_cast<void>(ImGui::SliderInt("##interval", &interval_ms_, kMinInterval, kMaxInterval, "%d ms",
                                       ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp));
    if (ImGui::IsItemDeactivatedAfterEdit() && state.monitoring) {
        start_monitor();  // The new interval applies at once.
    }
    ImGui::SameLine();
    if (widget_mode_) {
        draw_shown_choice(state);
    } else {
        Text count{};
        static_cast<void>(common::append_unsigned(count, selected_count()) && count.append(" of ") &&
                          common::append_unsigned(count, kMaxMonitored) && count.append(" selected"));
        show_disabled(count.view());
    }
}

void App::draw_shown_choice(const State& state) noexcept {
    Text preview{};
    static_cast<void>(common::append_unsigned(preview, selected_count()) && preview.append(" of ") &&
                      common::append_unsigned(preview, kMaxMonitored) && preview.append(" shown"));
    label("Show");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0F);
    if (ImGui::BeginCombo("##shown", preview.c_str(), ImGuiComboFlags_HeightLargest)) {
        const bool full{selected_count() >= kMaxMonitored};
        ImGui::SeparatorText("Gauges");
        for (const GaugeSpec& spec : gauges()) {
            if (state.supported_pids.contains(spec.pid)) {
                const common::Result<PidInfo> info{find_pid(spec.pid)};
                Text text{};
                static_cast<void>(text.append(spec.title) && text.append("  (") &&
                                  text.append(info.has_value() ? info.value().name : std::string_view{}) &&
                                  text.append(")"));
                bool selected{selected_[spec.pid]};
                ImGui::PushID(static_cast<int>(spec.pid));
                ImGui::BeginDisabled(full && (!selected));
                if (ImGui::Checkbox(text.c_str(), &selected)) {
                    select(spec.pid, selected);
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::SeparatorText("Digital displays");
        for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
            const std::uint8_t pid{static_cast<std::uint8_t>(value)};
            if (state.supported_pids.contains(pid) && (!is_support_pid(pid)) && (find_gauge(pid) == nullptr)) {
                const common::Result<PidInfo> info{find_pid(pid)};
                Text text{hex_text(pid, 2U)};
                static_cast<void>(text.append("  ") &&
                                  text.append(info.has_value() ? info.value().name : std::string_view{"(unknown PID)"}));
                bool selected{selected_[value]};
                ImGui::PushID(static_cast<int>(value));
                ImGui::BeginDisabled(full && (!selected));
                if (ImGui::Checkbox(text.c_str(), &selected)) {
                    select(pid, selected);
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Default dashboard")) {
            const common::ByteSet defaults{default_selection(state.supported_pids, kMaxMonitored)};
            for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
                selected_[value] = defaults.contains(static_cast<std::uint8_t>(value));
            }
            if (state.monitoring) {
                start_monitor();
            }
        }
        ImGui::EndCombo();
    }
}

void App::draw_pid_table(const State& state) noexcept {
    const ImGuiTableFlags flags{ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_SizingFixedFit};
    if (ImGui::BeginTable("pid_table", 5, flags)) {
        const float font{ImGui::GetFontSize()};
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, font * 7.0F);
        ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, font * 3.0F);
        ImGui::TableHeadersRow();
        const bool full{selected_count() >= kMaxMonitored};
        for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
            const std::uint8_t pid{static_cast<std::uint8_t>(value)};
            // The support PIDs 20, 40, ... only link the ranges.
            if (state.supported_pids.contains(pid) && (!is_support_pid(pid))) {
                const common::Result<PidInfo> info{find_pid(pid)};
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(value));
                static_cast<void>(ImGui::TableNextColumn());
                ImGui::BeginDisabled(full && (!selected_[value]));
                if (ImGui::Checkbox("##plot", &selected_[value]) && state.monitoring) {
                    start_monitor();
                }
                ImGui::EndDisabled();
                static_cast<void>(ImGui::TableNextColumn());
                show(hex_text(pid, 2U).text());
                static_cast<void>(ImGui::TableNextColumn());
                show(info.has_value() ? info.value().name : std::string_view{"(unknown PID)"});
                static_cast<void>(ImGui::TableNextColumn());
                if (has_latest_[value]) {
                    const PidReading& reading{latest_[value]};
                    if (is_ok(reading.status)) {
                        show(value_text(reading, false).text());
                    } else {
                        show_disabled(std::string_view{common::to_string(reading.status)});
                    }
                }
                static_cast<void>(ImGui::TableNextColumn());
                show(info.has_value() ? info.value().unit : std::string_view{});
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void App::draw_plots() noexcept {
    const float height{ImGui::GetFontSize() * 4.0F};
    bool any{false};
    for (const History& history : histories_) {
        if (history.used) {
            any = true;
            const common::Result<PidInfo> info{find_pid(history.pid)};
            ImGui::PushID(static_cast<int>(history.pid));
            Text title{hex_text(history.pid, 2U)};
            static_cast<void>(title.append("  ") &&
                              title.append(info.has_value() ? info.value().name : std::string_view{"(unknown PID)"}));
            show(kBlue, title.text());
            Text overlay{};
            if (has_latest_[history.pid] && is_ok(latest_[history.pid].status)) {
                overlay = value_text(latest_[history.pid], true);
            }
            if (history.count > 0U) {
                const std::size_t offset{(history.count < kHistorySize) ? 0U : history.next};
                ImGui::PushStyleColor(ImGuiCol_PlotLines, kBlue);
                ImGui::PlotLines("##values", history.values.data(), static_cast<int>(history.count),
                                 static_cast<int>(offset), overlay.c_str(), std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::max(), ImVec2{-1.0F, height});
                ImGui::PopStyleColor();
            } else {
                show_disabled("Waiting for values...");
            }
            ImGui::PopID();
        }
    }
    if (!any) {
        show_disabled("Tick PIDs in the table and press 'Monitor selected' to plot them.");
    }
}

void App::draw_trouble_codes() noexcept {
    const State& state{worker_->state()};
    // Read once without a click when the tab opens: reading the codes changes nothing.
    if (state.connected && (!state.dtcs_read) && (!dtcs_requested_) && (!is_busy(state))) {
        dtcs_requested_ = worker_->post(RequestKind::kReadDtcs);
    }
    ImGui::BeginDisabled((!state.connected) || is_busy(state));
    if (ImGui::Button("Read trouble codes")) {
        static_cast<void>(worker_->post(RequestKind::kReadDtcs));
    }
    ImGui::EndDisabled();
    if (!state.dtcs_read) {
        show_disabled(state.connected ? "Not read yet." : "Connect to the adapter to read the trouble codes.");
    } else {
        if (state.mil_known) {
            label("Malfunction indicator lamp (MIL)");
            show(state.mil.on ? kRed : kGreen, state.mil.on ? "on" : "off");
            ImGui::SameLine();
            Text count{};
            static_cast<void>(count.append("(") && common::append_unsigned(count, state.mil.dtc_count) &&
                              count.append((state.mil.dtc_count == 1U) ? " confirmed DTC)" : " confirmed DTCs)"));
            show(count.text());
        }
        for (std::size_t index{0U}; index < state.dtcs.size(); ++index) {
            const DtcSection& section{state.dtcs[index]};
            ImGui::SeparatorText(kDtcLabels[index]);
            if (section.status == common::Status::kNotFound) {
                show_disabled("No answer: some ECUs stay silent when they have no codes of the kind.");
            } else if (section.status == common::Status::kNotSupported) {
                show_disabled("Not supported by the vehicle.");
            } else if (!is_ok(section.status)) {
                show(kRed, std::string_view{common::to_string(section.status)});
            } else if (section.codes.empty()) {
                show(kGreen, "None");
            } else {
                for (const Dtc dtc : section.codes) {
                    std::array<char, kDtcTextSize> code{};
                    const common::Result<std::size_t> length{format_dtc(dtc, code)};
                    show(state.mil.on ? kYellow : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                         common::Span<const char>{code.data(), length.value_or(0U)});
                    ImGui::SameLine(ImGui::GetFontSize() * 5.0F);
                    show_disabled(std::string_view{kDtcSystems[static_cast<std::uint32_t>(dtc) >> kDtcSystemShift]});
                }
            }
        }
    }
}

void App::draw_vehicle() noexcept {
    const State& state{worker_->state()};
    if (state.connected && (!state.vin_read) && (!vin_requested_) && (!is_busy(state))) {
        vin_requested_ = worker_->post(RequestKind::kReadVin);
    }
    const bool idle{state.connected && (!is_busy(state))};
    const ImGuiTableFlags flags{ImGuiTableFlags_SizingFixedFit};
    ImGui::SeparatorText("Vehicle");
    if (ImGui::BeginTable("vehicle", 2, flags)) {
        ImGui::TableNextRow();
        static_cast<void>(ImGui::TableNextColumn());
        show("VIN");
        static_cast<void>(ImGui::TableNextColumn());
        if (!state.vin_read) {
            show_disabled("not read yet");
        } else if (is_ok(state.vin_status)) {
            show(state.vin.text());
        } else {
            show(kRed, std::string_view{common::to_string(state.vin_status)});
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!idle);
    if (ImGui::Button("Read VIN")) {
        static_cast<void>(worker_->post(RequestKind::kReadVin));
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Adapter");
    if (ImGui::BeginTable("adapter", 2, flags)) {
        const std::array<const char*, 4U> labels{{"Identity", "Description", "Voltage", "Protocol"}};
        const std::array<const AdapterText*, 4U> values{
            {&state.identity, &state.description, &state.voltage, &state.protocol}};
        for (std::size_t index{0U}; index < labels.size(); ++index) {
            ImGui::TableNextRow();
            static_cast<void>(ImGui::TableNextColumn());
            show(std::string_view{labels[index]});
            static_cast<void>(ImGui::TableNextColumn());
            if (values[index]->empty()) {
                show_disabled("-");
            } else {
                show(values[index]->text());
            }
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!idle);
    if (ImGui::Button("Refresh")) {
        static_cast<void>(worker_->post(RequestKind::kReadInfo));
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Connection");
    if (ImGui::BeginTable("connection", 2, flags)) {
        const ConnectionSettings& settings{state.settings};
        Text baud{};
        static_cast<void>(common::append_unsigned(baud, static_cast<std::uint32_t>(settings.baud_rate)) &&
                          baud.append(" bit/s"));
        Text protocol{};
        static_cast<void>(protocol.append(settings.protocol) && protocol.append(" (") &&
                          protocol.append(std::string_view{protocol_name(settings.protocol)}) &&
                          protocol.append(")"));
        const std::array<const char*, 3U> labels{{"Device", "Baud rate", "Protocol setting"}};
        const std::array<common::Span<const char>, 3U> values{{settings.device.text(), baud.text(), protocol.text()}};
        for (std::size_t index{0U}; index < labels.size(); ++index) {
            ImGui::TableNextRow();
            static_cast<void>(ImGui::TableNextColumn());
            show(std::string_view{labels[index]});
            static_cast<void>(ImGui::TableNextColumn());
            show(values[index]);
        }
        ImGui::EndTable();
    }
}

void App::draw_terminal() noexcept {
    const State& state{worker_->state()};
    const float footer{ImGui::GetFrameHeightWithSpacing()};
    if (ImGui::BeginChild("log", ImVec2{0.0F, -footer}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper{};
        clipper.Begin(static_cast<int>(log_count_));
        const std::size_t first{(log_next_ + kLogSize) - log_count_};
        while (clipper.Step()) {
            for (int row{clipper.DisplayStart}; row < clipper.DisplayEnd; ++row) {
                const TraceLine& line{log_[(first + static_cast<std::size_t>(row)) % kLogSize]};
                switch (line.kind) {
                    case TraceLine::Kind::kCommand: {
                        Text text{};
                        static_cast<void>(text.append("> ") && text.append(line.text.text()));
                        show(kBlue, text.text());
                        break;
                    }
                    case TraceLine::Kind::kReply:
                        show(line.text.text());
                        break;
                    case TraceLine::Kind::kNote:
                        show(kYellow, line.text.text());
                        break;
                    case TraceLine::Kind::kError:
                        show(kRed, line.text.text());
                        break;
                }
            }
        }
        clipper.End();
        // Follow new lines while the view is at the bottom.
        if (scroll_log_ || (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) {
            ImGui::SetScrollHereY(1.0F);
        }
        scroll_log_ = false;
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(!state.connected);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * -9.0F);
    if (ImGui::InputTextWithHint("##command", "AT command or OBD request, e.g. ATRV or 010C", command_.data(),
                                 command_.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        send_command();
        ImGui::SetKeyboardFocusHere(-1);  // Keep typing commands.
    }
    ImGui::SameLine();
    if (ImGui::Button("Send")) {
        send_command();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        log_count_ = 0U;
    }
}

void App::draw_expert() noexcept {
    const State& state{worker_->state()};
    if (state.read_only) {
        show_wrapped(kGreen, "Read-only mode is on: the requests that could change the vehicle are disabled here, "
                             "and refused if sent anyway.");
    } else {
        show_wrapped(kYellow, "Expert mode sends UDS/KWP requests to an ECU. A write can change or disable a "
                              "feature: each one is confirmed first, and a coding write backs up the value it "
                              "changes. Turn on Read-only in the bar above to explore safely.");
    }
    const float font{ImGui::GetFontSize()};
    if (ImGui::BeginChild("expert_controls", ImVec2{font * 31.0F, 0.0F}, ImGuiChildFlags_Borders)) {
        draw_expert_controls(state);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("expert_results", ImVec2{0.0F, 0.0F})) {
        draw_expert_results();
        draw_coding_reference();
    }
    ImGui::EndChild();
    draw_confirmation();
}

void App::draw_expert_controls(const State& state) noexcept {
    const bool idle{state.connected && (!is_busy(state))};
    const bool writes{idle && (!state.read_only)};  // The controls that can change the vehicle.
    const float font{ImGui::GetFontSize()};
    const std::string_view read_only_note{"Disabled in read-only mode."};
    if (!state.connected) {
        show_disabled("Connect to the adapter to talk to an ECU.");
    }

    ImGui::SeparatorText("Target ECU");
    ImGui::BeginDisabled(!idle);
    label("Header");
    ImGui::SetNextItemWidth(font * 5.0F);
    static_cast<void>(
        ImGui::InputTextWithHint("##header", "7E0", target_header_.data(), target_header_.size(), kHexFlags));
    ImGui::SameLine();
    label("Reply");
    ImGui::SetNextItemWidth(font * 5.0F);
    static_cast<void>(
        ImGui::InputTextWithHint("##reply", "any", target_reply_.data(), target_reply_.size(), kHexFlags));
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        apply_target();
    }
    ImGui::EndDisabled();
    tooltip("The CAN identifier the requests go to (ATSH) and the one the replies come from (ATCRA, empty for "
            "any), e.g. 7E0 and 7E8 for the engine ECU. Without a header the requests keep the adapter's. This "
            "configures the adapter only.");

    ImGui::SeparatorText("Session");
    ImGui::BeginDisabled(!writes);
    ImGui::SetNextItemWidth(font * 10.0F);
    static_cast<void>(ImGui::Combo("##session", &session_index_, kSessionNames.data(),
                                   static_cast<int>(kSessionNames.size())));
    ImGui::SameLine();
    if (ImGui::Button("Change...")) {
        change_session();
    }
    ImGui::EndDisabled();
    tooltip(state.read_only ? read_only_note
                            : std::string_view{"DiagnosticSessionControl (10). Coding often needs the extended "
                                               "session; the programming session is for flashing."});
    ImGui::SameLine();
    ImGui::BeginDisabled(!idle);
    if (ImGui::Button("Tester present")) {
        tester_present();
    }
    ImGui::EndDisabled();
    tooltip("TesterPresent (3E): checks that the ECU answers, and keeps a session open. Changes nothing.");

    ImGui::SeparatorText("Data identifier");
    ImGui::BeginDisabled(!idle);
    label("DID");
    ImGui::SetNextItemWidth(font * 5.0F);
    static_cast<void>(ImGui::InputTextWithHint("##did", "F190", did_.data(), did_.size(), kHexFlags));
    ImGui::SameLine();
    if (ImGui::Button("Read")) {
        read_did();
    }
    ImGui::EndDisabled();
    tooltip("ReadDataByIdentifier (22): reads a value, e.g. F190 (the VIN), F187 (the part number) or a "
            "coding block. Changes nothing.");
    ImGui::BeginDisabled(!writes);
    label("Value");
    ImGui::SetNextItemWidth(font * -6.0F);
    static_cast<void>(ImGui::InputTextWithHint("##value", "hex bytes, e.g. 01 or 0A 1B", value_.data(),
                                               value_.size(), kBytesFlags));
    ImGui::SameLine();
    if (ImGui::Button("Write...")) {
        write_did();
    }
    ImGui::EndDisabled();
    tooltip(state.read_only ? read_only_note
                            : std::string_view{"WriteDataByIdentifier (2E): coding and adaptation. It reads the "
                                               "current value first, and writes only when it could, so the "
                                               "value can be restored."});

    ImGui::SeparatorText("Routine");
    ImGui::BeginDisabled(!writes);
    ImGui::SetNextItemWidth(font * 7.0F);
    static_cast<void>(ImGui::Combo("##routine_type", &routine_type_index_, kRoutineNames.data(),
                                   static_cast<int>(kRoutineNames.size())));
    ImGui::SameLine();
    label("ID");
    ImGui::SetNextItemWidth(font * 5.0F);
    static_cast<void>(ImGui::InputTextWithHint("##routine", "FF00", routine_.data(), routine_.size(), kHexFlags));
    label("Data");
    ImGui::SetNextItemWidth(font * -5.0F);
    static_cast<void>(ImGui::InputTextWithHint("##routine_data", "optional hex bytes", routine_data_.data(),
                                               routine_data_.size(), kBytesFlags));
    ImGui::SameLine();
    if (ImGui::Button("Run...")) {
        run_routine();
    }
    ImGui::EndDisabled();
    tooltip(state.read_only ? read_only_note
                            : std::string_view{"RoutineControl (31): starts, stops or polls a routine of the ECU, "
                                               "e.g. a calibration or an actuator test."});

    ImGui::SeparatorText("Security access");
    ImGui::BeginDisabled(!writes);
    label("Level");
    ImGui::SetNextItemWidth(font * 3.0F);
    static_cast<void>(ImGui::InputTextWithHint("##level", "01", security_level_.data(), security_level_.size(),
                                               kHexFlags));
    ImGui::SameLine();
    if (ImGui::Button("Request seed...")) {
        request_seed();
    }
    label("Key");
    ImGui::SetNextItemWidth(font * -7.0F);
    static_cast<void>(ImGui::InputTextWithHint("##key", "from your own tooling", security_key_.data(),
                                               security_key_.size(), kBytesFlags));
    ImGui::SameLine();
    if (ImGui::Button("Send key...")) {
        send_key();
    }
    ImGui::EndDisabled();
    show_note("SecurityAccess (27): the seed comes at the odd level, the key goes back at the next one. The key "
              "is computed from the seed by your own licensed tooling, never here.");

    ImGui::SeparatorText("ECU reset");
    ImGui::BeginDisabled(!writes);
    ImGui::SetNextItemWidth(font * 9.0F);
    static_cast<void>(
        ImGui::Combo("##reset", &reset_index_, kResetNames.data(), static_cast<int>(kResetNames.size())));
    ImGui::SameLine();
    if (ImGui::Button("Reset ECU...")) {
        reset_ecu();
    }
    ImGui::EndDisabled();
    tooltip(state.read_only ? read_only_note
                            : std::string_view{"ECUReset (11): restarts the ECU at once. Never while driving."});

    ImGui::SeparatorText("Any request");
    const std::string_view typed{view_of(any_request_)};
    const Access access{command_access(typed)};
    const bool refused{state.read_only && (access != Access::kRead)};
    ImGui::BeginDisabled(!idle);
    ImGui::SetNextItemWidth(font * -5.0F);
    const bool entered{ImGui::InputTextWithHint("##any", "hex, e.g. 22 F1 90", any_request_.data(),
                                                any_request_.size(),
                                                kBytesFlags | ImGuiInputTextFlags_EnterReturnsTrue)};
    ImGui::SameLine();
    ImGui::BeginDisabled(refused);
    const bool clicked{ImGui::Button("Send")};
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if ((clicked || entered) && idle && (!refused)) {
        send_any();
    }
    if (!typed.empty()) {
        // How the read-only gate sees the request.
        Text text{};
        static_cast<void>(text.append("Classified as ") && text.append(std::string_view{elm327::to_string(access)}));
        const ImVec4 color{(access == Access::kRead) ? kGreen : ((access == Access::kWrite) ? kYellow : kRed)};
        show(color, text.text());
        if (refused) {
            ImGui::SameLine();
            show(kGreen, "- refused in read-only mode");
        } else if (access != Access::kRead) {
            ImGui::SameLine();
            show_disabled("- asks for confirmation");
        } else {
            // A read goes at once.
        }
    }
    if (!expert_error_.empty()) {
        ImGui::Spacing();
        show_wrapped(kRed, expert_error_.text());
    }
}

void App::draw_expert_results() noexcept {
    const float font{ImGui::GetFontSize()};
    ImGui::SeparatorText("Results");
    // The coding reference below takes half the room when it is open.
    const float available{ImGui::GetContentRegionAvail().y};
    const float reserve{coding_open_ ? (available * 0.5F) : (ImGui::GetFrameHeightWithSpacing() * 2.5F)};
    const float minimum{font * 6.0F};
    const float height{((available - reserve) > minimum) ? (available - reserve) : minimum};
    const ImGuiTableFlags flags{ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable};
    if (ImGui::BeginTable("uds_results", 4, flags, ImVec2{0.0F, height})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("For", ImGuiTableColumnFlags_WidthFixed, font * 6.5F);
        ImGui::TableSetupColumn("Request", ImGuiTableColumnFlags_WidthFixed, font * 8.5F);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, font * 4.5F);
        ImGui::TableSetupColumn("Reply", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        if (uds_log_count_ == 0U) {
            ImGui::TableNextRow();
            static_cast<void>(ImGui::TableNextColumn());
            show_disabled("None yet.");
        }
        const std::size_t first{(uds_log_next_ + kUdsLogSize) - uds_log_count_};
        for (std::size_t row{0U}; row < uds_log_count_; ++row) {
            const UdsResult& result{uds_log_[(first + row) % kUdsLogSize]};
            ImGui::TableNextRow();
            static_cast<void>(ImGui::TableNextColumn());
            show(result.label.text());
            static_cast<void>(ImGui::TableNextColumn());
            show(kBlue, result.request.text());
            static_cast<void>(ImGui::TableNextColumn());
            show(outcome_color(result.outcome), std::string_view{outcome_text(result.outcome)});
            static_cast<void>(ImGui::TableNextColumn());
            if (result.outcome == UdsResult::Outcome::kPositive) {
                const LongText reply{reply_text(result.exchange)};
                show_wrapped(reply.view());
            } else {
                show_wrapped(outcome_color(result.outcome), result.reason.text());
            }
        }
        if (scroll_uds_log_) {
            ImGui::SetScrollHereY(1.0F);  // Show the newest result.
            scroll_uds_log_ = false;
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(uds_log_count_ == 0U);
    if (ImGui::Button("Clear results")) {
        uds_log_count_ = 0U;
    }
    ImGui::EndDisabled();
}

void App::draw_coding_reference() noexcept {
    coding_open_ = ImGui::CollapsingHeader("Coding reference: VAG, Renault, BMW");
    if (coding_open_) {
        const float font{ImGui::GetFontSize()};
        label("Make");
        ImGui::SetNextItemWidth(font * 8.0F);
        static_cast<void>(
            ImGui::Combo("##make", &coding_make_index_, kMakeNames.data(), static_cast<int>(kMakeNames.size())));
        show_note("Where a feature usually lives and how it is coded -- not the values, which differ by model, "
                  "year and firmware. Look them up for your own vehicle, read the current value, change one "
                  "documented field, and keep the original to restore it.");
        const bool all{coding_make_index_ <= 0};
        const expert::Make wanted{item_of(kMakes, coding_make_index_ - 1)};
        const float available{ImGui::GetContentRegionAvail().y};
        const float height{(available > (font * 6.0F)) ? available : (font * 6.0F)};
        const ImGuiTableFlags flags{ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable};
        if (ImGui::BeginTable("coding", 5, flags, ImVec2{0.0F, height})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Make", ImGuiTableColumnFlags_WidthFixed, font * 4.5F);
            ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthStretch, 1.2F);
            ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch, 1.2F);
            ImGui::TableSetupColumn("How", ImGuiTableColumnFlags_WidthStretch, 1.5F);  // Fits "WriteDataByIdentifier".
            ImGui::TableSetupColumn("Check on your car", ImGuiTableColumnFlags_WidthStretch, 2.3F);
            ImGui::TableHeadersRow();
            for (const expert::CodingHint& hint : expert::coding_catalog()) {
                if (all || (hint.make == wanted)) {
                    ImGui::TableNextRow();
                    static_cast<void>(ImGui::TableNextColumn());
                    show(std::string_view{expert::make_name(hint.make)});
                    static_cast<void>(ImGui::TableNextColumn());
                    show_wrapped(hint.feature);
                    static_cast<void>(ImGui::TableNextColumn());
                    show_wrapped(hint.module);
                    static_cast<void>(ImGui::TableNextColumn());
                    show_wrapped(hint.method);
                    static_cast<void>(ImGui::TableNextColumn());
                    show_wrapped(hint.note);
                }
            }
            ImGui::EndTable();
        }
    }
}

void App::draw_confirmation() noexcept {
    constexpr const char* kTitle{"Confirm a request that can change the vehicle"};
    if (open_confirmation_) {
        ImGui::OpenPopup(kTitle);
        open_confirmation_ = false;
        confirmed_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2{0.5F, 0.5F});
    if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const float font{ImGui::GetFontSize()};
        ImGui::PushTextWrapPos(font * 32.0F);
        show(kYellow, "This request can change the vehicle.");
        ImGui::Spacing();
        show(pending_summary_.text());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        static_cast<void>(ImGui::Checkbox("I have backed up what this changes, and I accept the risk.", &confirmed_));
        ImGui::Spacing();
        ImGui::BeginDisabled(!confirmed_);
        if (ImGui::Button("Send", ImVec2{font * 6.0F, 0.0F})) {
            static_cast<void>(worker_->post(pending_));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{font * 6.0F, 0.0F})) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::draw_status() noexcept {
    const State& state{worker_->state()};
    ImGui::Separator();
    if (state.activity == Activity::kIdle) {
        show_disabled("Ready");
    } else {
        Text text{};
        const std::size_t frame{static_cast<std::size_t>(ImGui::GetTime() * 8.0) % kSpinner.size()};
        static_cast<void>(text.append(kSpinner[frame]) && text.append(" ") &&
                          text.append(std::string_view{to_string(state.activity)}) &&
                          text.append((state.activity == Activity::kMonitoring) ? "" : "..."));
        show(kYellow, text.text());
    }
    if (!state.message.empty()) {
        ImGui::SameLine(ImGui::GetFontSize() * 14.0F);
        if (state.message_failed) {
            show(kRed, state.message.text());
        } else {
            show(state.message.text());
        }
    }
    const std::uint64_t dropped{worker_->dropped()};
    if (dropped > 0U) {
        ImGui::SameLine();
        Text text{};
        static_cast<void>(text.append("(") && common::append_unsigned(text, dropped) && text.append(" lost)"));
        show_disabled(text.view());
    }
}

void App::connect() noexcept {
    Request request{};
    request.kind = RequestKind::kConnect;
    request.settings = settings();
    request.read_only = read_only_;
    static_cast<void>(worker_->post(request));
}

void App::start_monitor() noexcept {
    const common::ByteSet pids{selection()};
    reset_histories(pids);
    Request request{};
    request.kind = RequestKind::kStartMonitor;
    request.pids = pids;
    request.interval = std::chrono::milliseconds{interval_ms_};
    static_cast<void>(worker_->post(request));
}

void App::start_dashboard() noexcept {
    const State& state{worker_->state()};
    if (state.connected && state.pids_known) {
        // Keep a selection with a gauge in it; otherwise show the default dashboard.
        bool gauge{false};
        for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
            gauge = gauge || (selected_[value] && (find_gauge(static_cast<std::uint8_t>(value)) != nullptr));
        }
        if (!gauge) {
            const common::ByteSet defaults{default_selection(state.supported_pids, kMaxMonitored)};
            for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
                selected_[value] = defaults.contains(static_cast<std::uint8_t>(value));
            }
        }
        if (((!gauge) || (!state.monitoring)) && (selected_count() > 0U)) {
            start_monitor();
        }
        dashboard_started_ = true;
    }
}

void App::select(const std::uint8_t pid, const bool selected) noexcept {
    selected_[pid] = selected;
    if (worker_->state().monitoring) {
        start_monitor();  // The monitor follows the selection.
    }
}

void App::send_command() noexcept {
    const common::Span<const char> text{common::trim(
        common::Span<const char>{view_of(command_).data(), view_of(command_).size()})};
    Request request{};
    request.kind = RequestKind::kRaw;
    if ((!text.empty()) && request.command.append(text)) {
        static_cast<void>(worker_->post(request));
        command_[0U] = '\0';
        scroll_log_ = true;
    }
}

void App::submit(const Request& request, const bool writes, const common::Span<const char> summary) noexcept {
    expert_error_.clear();
    if (writes) {
        pending_ = request;
        pending_summary_.clear();
        static_cast<void>(pending_summary_.append(summary));
        open_confirmation_ = true;
    } else {
        static_cast<void>(worker_->post(request));
    }
}

void App::apply_target() noexcept {
    const std::string_view header{view_of(target_header_)};
    const std::string_view reply{view_of(target_reply_)};
    const bool header_valid{header.empty() || (header.size() == 3U) || (header.size() == 6U) || (header.size() == 8U)};
    const bool reply_valid{reply.empty() || (reply.size() == 3U) || (reply.size() == 8U)};
    if ((!header_valid) || (!reply_valid)) {
        say(expert_error_, "A header has 3, 6 or 8 hexadecimal digits (e.g. 7E0), a reply address 3 or 8 (e.g. 7E8).");
    } else {
        expert_error_.clear();
        Request request{};
        request.kind = RequestKind::kRaw;
        if (!header.empty()) {
            static_cast<void>(request.command.append("ATSH") && request.command.append(header));
            static_cast<void>(worker_->post(request));
        }
        request.command.clear();
        static_cast<void>(request.command.append("ATCRA") && request.command.append(reply));  // Alone: any reply.
        static_cast<void>(worker_->post(request));
    }
}

void App::change_session() noexcept {
    expert::UdsCommand command{};
    static_cast<void>(expert::uds_session_control(item_of(kSessions, session_index_), command));
    LongText summary{};
    static_cast<void>(summary.append("Change the diagnostic session to ") &&
                      summary.append(std::string_view{item_of(kSessionNames, session_index_)}) &&
                      summary.append(", with ") && summary.append(command.view()) &&
                      summary.append(". Outside the default session some functions of the ECU stop until it "
                                     "returns to it; the programming session is meant for flashing."));
    submit(make_uds_request("session", command.view()), true, summary.text());
}

void App::tester_present() noexcept {
    expert::UdsCommand command{};
    static_cast<void>(expert::uds_tester_present(false, command));  // Ask for the answer.
    submit(make_uds_request("tester present", command.view()), false, common::Span<const char>{});
}

void App::read_did() noexcept {
    const common::Result<std::uint16_t> did{parse_number(view_of(did_), 4U)};
    if (!did.has_value()) {
        say(expert_error_, "A DID has 1 to 4 hexadecimal digits, e.g. F190.");
    } else {
        expert::UdsCommand command{};
        static_cast<void>(expert::uds_read_data_by_identifier(did.value(), command));
        submit(make_uds_request("read", command.view()), false, common::Span<const char>{});
    }
}

void App::write_did() noexcept {
    const common::Result<std::uint16_t> did{parse_number(view_of(did_), 4U)};
    const common::Result<HexBytes> value{parse_bytes(view_of(value_))};
    if (!did.has_value()) {
        say(expert_error_, "A DID has 1 to 4 hexadecimal digits, e.g. F190.");
    } else if ((!value.has_value()) || value.value().empty() || (value.value().size() > expert::kMaxRequestData)) {
        say(expert_error_, "The value is 1 to 29 bytes in hexadecimal, e.g. 01 or 0A 1B.");
    } else {
        Request request{};
        request.kind = RequestKind::kUdsWrite;
        request.did = did.value();
        for (const std::uint8_t byte : value.value()) {
            static_cast<void>(request.value.push_back(byte));  // Cannot fail: the size is checked above.
        }
        const common::Span<const std::uint8_t> bytes{value.value().data(), value.value().size()};
        LongText summary{};
        static_cast<void>(
            summary.append("Write ") && common::append_hex_bytes(summary, bytes, ' ') &&
            summary.append(" to the data identifier ") && common::append_hex(summary, did.value(), 4U) &&
            summary.append(" (WriteDataByIdentifier). The current value is read first and kept in the results; if "
                           "it cannot be read, nothing is written. The value is read again after the write."));
        submit(request, true, summary.text());
    }
}

void App::run_routine() noexcept {
    const common::Result<std::uint16_t> routine{parse_number(view_of(routine_), 4U)};
    const common::Result<HexBytes> data{parse_bytes(view_of(routine_data_))};
    const expert::RoutineControlType type{item_of(kRoutineTypes, routine_type_index_)};
    expert::UdsCommand command{};
    bool built{false};
    if (routine.has_value() && data.has_value()) {
        built = expert::uds_routine_control(
            type, routine.value(), common::Span<const std::uint8_t>{data.value().data(), data.value().size()}, command);
    }
    if (!routine.has_value()) {
        say(expert_error_, "A routine ID has 1 to 4 hexadecimal digits, e.g. FF00.");
    } else if (!built) {
        say(expert_error_, "The data is hexadecimal bytes, few enough for one request.");
    } else {
        LongText summary{};
        static_cast<void>(summary.append("RoutineControl ") &&
                          summary.append(std::string_view{item_of(kRoutineNames, routine_type_index_)}) &&
                          summary.append(" of routine ") && common::append_hex(summary, routine.value(), 4U) &&
                          summary.append(", with ") && summary.append(command.view()) &&
                          summary.append(". A routine can move actuators, run a calibration or erase learned values: "
                                         "run it only as its documentation says, with the vehicle standing."));
        submit(make_uds_request("routine", command.view()), true, summary.text());
    }
}

void App::request_seed() noexcept {
    const common::Result<std::uint16_t> level{parse_number(view_of(security_level_), 2U)};
    if ((!level.has_value()) || ((static_cast<std::uint32_t>(level.value()) % 2U) == 0U) ||
        (level.value() > kMaxSeedLevel)) {
        say(expert_error_, "The level of a seed is odd, 01 to 7D, e.g. 01; the key goes back at the next level.");
    } else {
        expert::UdsCommand command{};
        static_cast<void>(expert::uds_security_request_seed(static_cast<std::uint8_t>(level.value()), command));
        LongText summary{};
        static_cast<void>(summary.append("Request a security seed at level ") &&
                          common::append_hex(summary, level.value(), 2U) && summary.append(", with ") &&
                          summary.append(command.view()) &&
                          summary.append(". The ECU answers with a seed; the key for it comes from your own tooling. "
                                         "This starts the unlock sequence, and some ECUs count the attempts."));
        submit(make_uds_request("security seed", command.view()), true, summary.text());
    }
}

void App::send_key() noexcept {
    const common::Result<std::uint16_t> level{parse_number(view_of(security_level_), 2U)};
    const common::Result<HexBytes> key{parse_bytes(view_of(security_key_))};
    if ((!level.has_value()) || ((static_cast<std::uint32_t>(level.value()) % 2U) == 0U) ||
        (level.value() > kMaxSeedLevel)) {
        say(expert_error_, "The level of the seed is odd, 01 to 7D; the key goes back at the next level.");
    } else if ((!key.has_value()) || key.value().empty()) {
        say(expert_error_, "The key is hexadecimal bytes, as your own tooling computed it from the seed.");
    } else {
        const std::uint8_t key_level{static_cast<std::uint8_t>(level.value() + 1U)};
        expert::UdsCommand command{};
        if (!expert::uds_security_send_key(
                key_level, common::Span<const std::uint8_t>{key.value().data(), key.value().size()}, command)) {
            say(expert_error_, "The key is too long for one request.");
        } else {
            LongText summary{};
            static_cast<void>(summary.append("Send the key at level ") && common::append_hex(summary, key_level, 2U) &&
                              summary.append(", with ") && summary.append(command.view()) &&
                              summary.append(". A wrong key counts as a failed attempt; after a few, the ECU refuses "
                                             "access for a while (exceedNumberOfAttempts)."));
            submit(make_uds_request("security key", command.view()), true, summary.text());
        }
    }
}

void App::reset_ecu() noexcept {
    expert::UdsCommand command{};
    static_cast<void>(expert::uds_ecu_reset(item_of(kResets, reset_index_), command));
    LongText summary{};
    static_cast<void>(summary.append("Reset the ECU: ") &&
                      summary.append(std::string_view{item_of(kResetNames, reset_index_)}) &&
                      summary.append(", with ") && summary.append(command.view()) &&
                      summary.append(". It restarts at once, and may leave its session and forget learned values. "
                                     "Never do this while driving."));
    submit(make_uds_request("ecu reset", command.view()), true, summary.text());
}

void App::send_any() noexcept {
    const common::Result<HexBytes> bytes{parse_bytes(view_of(any_request_))};
    expert::UdsCommand command{};
    bool built{false};
    if (bytes.has_value() && (!bytes.value().empty())) {
        const common::Span<const std::uint8_t> all{bytes.value().data(), bytes.value().size()};
        built = expert::uds_request(all[0U], all.subspan(1U), command);
    }
    if (!built) {
        say(expert_error_, "Type the request in hexadecimal bytes, e.g. 22 F1 90 (at most 32 bytes).");
    } else {
        const Access access{command_access(command.view())};
        LongText summary{};
        static_cast<void>(summary.append("Send ") && summary.append(command.view()) && summary.append(" (") &&
                          summary.append(std::string_view{expert::uds_service_name(bytes.value()[0U])}) &&
                          summary.append("), which the read-only gate classifies as ") &&
                          summary.append(std::string_view{elm327::to_string(access)}) &&
                          summary.append(". Check what it does to your ECU before you send it."));
        submit(make_uds_request("request", command.view()), access != Access::kRead, summary.text());
    }
}

void App::reset_histories(const common::ByteSet& pids) noexcept {
    std::size_t slot{0U};
    for (std::uint32_t value{1U}; (value <= 0xFFU) && (slot < histories_.size()); ++value) {
        if (pids.contains(static_cast<std::uint8_t>(value))) {
            histories_[slot] = History{};
            histories_[slot].pid = static_cast<std::uint8_t>(value);
            histories_[slot].used = true;
            ++slot;
        }
    }
    for (; slot < histories_.size(); ++slot) {
        histories_[slot].used = false;
    }
}

void App::detect_devices() noexcept {
    detected_count_ = 0U;
    for (const std::string_view prefix : kDevicePrefixes) {
        for (const char number : kDeviceNumbers) {
            common::StaticString<ConnectionSettings::kMaxDeviceLength> path{};
            const bool built{path.append(prefix) && path.append(number)};
            if (built && (::access(path.c_str(), F_OK) == 0) && (detected_count_ < detected_.size())) {
                detected_[detected_count_] = path;
                ++detected_count_;
            }
        }
    }
}

common::ByteSet App::selection() const noexcept {
    common::ByteSet pids{};
    for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
        if (selected_[value]) {
            pids = pids.with(static_cast<std::uint8_t>(value));
        }
    }
    return pids;
}

std::size_t App::selected_count() const noexcept {
    std::size_t count{0U};
    for (const bool selected : selected_) {
        count += selected ? 1U : 0U;
    }
    return count;
}

ConnectionSettings App::settings() const noexcept {
    ConnectionSettings settings{};
    static_cast<void>(settings.device.assign(view_of(device_)));  // Cannot fail: the field has the same capacity.
    const std::size_t baud{static_cast<std::size_t>(baud_index_)};
    settings.baud_rate = (baud < kBaudRates.size()) ? kBaudRates[baud] : common::BaudRate::k38400;
    const std::size_t protocol{static_cast<std::size_t>(protocol_index_)};
    settings.protocol = (protocol < protocols().size()) ? protocols()[protocol] : '0';
    settings.timeouts.command = timeout_;
    return settings;
}

}  // namespace gui
}  // namespace elm327
