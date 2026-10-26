#include "elm327/controller/controller.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/protocol/text.hpp"
#include "common/time/deadline.hpp"

namespace elm327 {
namespace {

constexpr std::uint8_t kMilStatusPid{0x01U};

/// The first letters of an AT command, in either case.
constexpr std::string_view kAtFirst{"Aa"};
constexpr std::string_view kAtSecond{"Tt"};

bool is_at_command(const std::string_view command) noexcept {
    return (command.size() >= 2U) && (kAtFirst.find(command[0U]) != std::string_view::npos) &&
           (kAtSecond.find(command[1U]) != std::string_view::npos);
}

/// Appends @p text to the start of @p out at @p position. @return false if it does not fit.
bool put(const common::Span<const char> text, const common::Span<char> out, std::size_t& position) noexcept {
    const bool fits{text.size() <= (out.size() - position)};
    if (fits) {
        for (const char character : text) {
            out[position] = character;
            ++position;
        }
    }
    return fits;
}

}  // namespace

ConnectionSettings::ConnectionSettings() noexcept {
    // Cannot fail: the default path is far shorter than kMaxDeviceLength.
    static_cast<void>(device.assign(kDefaultDevice));
}

common::Result<std::size_t> format_reading(const PidReading& reading, const bool with_unit, const char separator,
                                           const common::Span<char> out) noexcept {
    using SizeResult = common::Result<std::size_t>;
    const common::Result<PidInfo> info{find_pid(reading.pid)};
    std::size_t position{0U};
    bool fits{true};
    if (reading.value.has_value() && info.has_value()) {
        std::array<char, Measurement::kMaxFormattedSize> text{};
        const SizeResult length{format_measurement(reading.value.value(), info.value().decimals, text)};
        fits = length.has_value() && put(common::Span<const char>{text.data(), length.value()}, out, position);
        const std::string_view unit{info.value().unit};
        if (fits && with_unit && (!unit.empty())) {
            fits = put(common::Span<const char>{" ", 1U}, out, position) &&
                   put(common::Span<const char>{unit.data(), unit.size()}, out, position);
        }
    } else {
        const common::Span<const std::uint8_t> data{reading.data.data(), reading.data.size()};
        const SizeResult length{common::format_hex_bytes(data, out, separator)};
        fits = length.has_value();
        position = length.value_or(0U);
    }
    return fits ? SizeResult::success(position) : SizeResult::failure(common::Status::kOutOfRange);
}

Controller::Controller() noexcept : obd_{adapter_} {}

common::Status Controller::connect(const ConnectionSettings& settings) noexcept {
    disconnect();
    settings_ = settings;
    response_.clear();
    query_last_ = true;
    AdapterOptions options{};
    options.serial.baud_rate = settings.baud_rate;
    options.timeouts = settings.timeouts;
    common::Status status{open(settings, options)};
    if (!is_ok(status)) {
        failure_ = ConnectFailure::kOpen;
    } else {
        status = adapter_.initialize(settings.protocol);
        if (is_ok(status)) {
            failure_ = ConnectFailure::kNone;
        } else {
            failure_ = (status == common::Status::kTimeout) ? ConnectFailure::kNoAnswer : ConnectFailure::kInitialize;
            // The port is closed in any case; an error here would change nothing for the caller.
            static_cast<void>(adapter_.close());
        }
    }
    return status;
}

common::Status Controller::open(const ConnectionSettings& settings, const AdapterOptions& options) noexcept {
#if defined(_WIN32)
    return adapter_.open(settings.device.c_str(), options);
#else
    common::Status status{common::Status::kNotSupported};
    if (link_opener_ != nullptr) {
        const common::Result<common::NativeHandle> link{link_opener_(
            settings.device.c_str(), common::Deadline::after(settings.timeouts.link), cancel_descriptor_, link_context_)};
        status = link.has_value() ? adapter_.open_stream(link.value(), options) : link.status();
    }
    if (status == common::Status::kNotSupported) {
        status = adapter_.open(settings.device.c_str(), options);  // Not a link: a serial device.
    }
    return status;
#endif
}

void Controller::disconnect() noexcept {
    // close() reports kOk when nothing is open, and the port is closed even if it reports an error.
    static_cast<void>(adapter_.close());
    knows_pids_ = false;
}

common::Status Controller::query(const std::string_view command, AdapterText& text) noexcept {
    text.clear();
    query_last_ = true;
    common::Status status{checked(adapter_.execute(command, response_), response_)};
    if (is_ok(status)) {
        if (response_.reply() == Reply::kContent) {
            // Cannot fail: a line has at most the capacity of AdapterText.
            static_cast<void>(text.append(response_.reply_line()));
        } else {
            status = common::Status::kNotSupported;  // E.g. "?".
        }
    }
    return status;
}

common::Status Controller::connect_vehicle() noexcept {
    common::Status status{common::Status::kOk};
    if (!knows_pids_) {
        query_last_ = false;
        status = checked(obd_.connect(), obd_.response());
    }
    return status;
}

common::Status Controller::read_supported_pids() noexcept {
    query_last_ = false;
    const common::Status status{checked(obd_.read_supported_pids(), obd_.response())};
    knows_pids_ = is_ok(status);
    return status;
}

common::Status Controller::ensure_supported_pids() noexcept {
    return knows_pids_ ? common::Status::kOk : read_supported_pids();
}

PidReading Controller::read_pid(const std::uint8_t pid) noexcept {
    query_last_ = false;
    PidReading reading{};
    reading.pid = pid;
    const common::Result<common::Span<const std::uint8_t>> data{obd_.read_pid(pid)};
    reading.status = checked(data.status(), obd_.response());
    if (data.has_value()) {
        const common::Span<const std::uint8_t> bytes{data.value()};
        for (std::size_t index{0U}; (index < bytes.size()) && (!reading.data.full()); ++index) {
            static_cast<void>(reading.data.push_back(bytes[index]));  // Cannot fail: the vector is not full.
        }
        const common::Result<PidInfo> info{find_pid(pid)};
        reading.value = info.has_value() ? decode_pid(info.value(), bytes)
                                         : common::Result<Measurement>::failure(common::Status::kNotFound);
    }
    return reading;
}

common::Result<MilStatus> Controller::read_mil_status() noexcept {
    const PidReading reading{read_pid(kMilStatusPid)};
    return is_ok(reading.status)
               ? decode_mil_status(common::Span<const std::uint8_t>{reading.data.data(), reading.data.size()})
               : common::Result<MilStatus>::failure(reading.status);
}

common::Status Controller::read_dtcs(const DtcKind kind, DtcList& dtcs) noexcept {
    query_last_ = false;
    return checked(obd_.read_dtcs(kind, dtcs), obd_.response());
}

common::Status Controller::read_vin(Vin& vin) noexcept {
    query_last_ = false;
    return checked(obd_.read_vin(vin), obd_.response());
}

common::Status Controller::clear_dtcs() noexcept {
    if (policy_ == AccessPolicy::kReadOnly) {
        return common::Status::kPermissionDenied;  // A write: refused before anything is sent.
    }
    query_last_ = false;
    return checked(obd_.clear_dtcs(), obd_.response());
}

common::Status Controller::execute(const std::string_view command, Response& response) noexcept {
    if (command_blocked(command, policy_)) {
        // Read-only mode: a command that could change the vehicle is refused before it is sent.
        response.clear();
        return common::Status::kPermissionDenied;
    }
    if (is_at_command(command)) {
        knows_pids_ = false;
    }
    // An OBD request may make the adapter search for the vehicle's protocol first.
    const common::Deadline deadline{common::Deadline::after(adapter_.timeouts().connect)};
    return checked(adapter_.execute(command, response, deadline), response);
}

common::Status Controller::checked(const common::Status status, const Response& response) noexcept {
    if ((status == common::Status::kDisconnected) && (!is_error(response.reply()))) {
        disconnect();
    }
    return status;
}

}  // namespace elm327
