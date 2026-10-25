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
#include "elm327/obd/dtc.hpp"
#include "elm327/obd/pid.hpp"
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
