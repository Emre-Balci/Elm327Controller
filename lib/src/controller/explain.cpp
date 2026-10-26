#include "elm327/controller/explain.hpp"

#include <string_view>

#include "elm327/adapter/adapter.hpp"

namespace elm327 {
namespace {

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

bool starts_with(const std::string_view text, const std::string_view prefix) noexcept {
    return (text.size() >= prefix.size()) && (std::string_view{text.data(), prefix.size()} == prefix);
}

/// The description of @p status, e.g. "timed out".
common::Span<const char> describe(const common::Status status) noexcept {
    return span_of(std::string_view{common::to_string(status)});
}

}  // namespace

Explanation explain_failure(const common::Status status, const Response& response) noexcept {
    Explanation explanation{};
    if (status == common::Status::kInterrupted) {
        explanation.reason = span_of("interrupted");
    } else if (is_error(response.reply())) {
        explanation.reason = response.reply_line();
        if (explanation.reason.empty()) {
            // The line itself did not fit in the Response.
            explanation.reason = span_of(std::string_view{to_string(response.reply())});
        }
        if (response.reply() == Reply::kUnableToConnect) {
            explanation.advice = "is the ignition on?";
        }
    } else if (status == common::Status::kInvalidArgument) {
        explanation.reason = span_of("unexpected reply");  // A reply that is not OBD data, or malformed.
    } else if (status == common::Status::kPermissionDenied) {
        explanation.reason = span_of("refused in read-only mode");
        explanation.advice = "turn read-only off to write to the vehicle";
    } else {
        explanation.reason = describe(status);
    }
    return explanation;
}

Explanation explain_open_failure(const common::Status status) noexcept {
    Explanation explanation{};
    switch (status) {
        case common::Status::kNotFound:
            explanation.reason = span_of("no such device");
            explanation.advice = "choose the adapter's serial device";
            break;
        case common::Status::kPermissionDenied:
            explanation.reason = span_of("permission denied");
#if defined(__linux__) && !defined(__ANDROID__)
            explanation.advice = "add your user to the dialout group";  // Only Linux has the group.
#endif
            break;
        case common::Status::kResourceLimit:
            explanation.reason = span_of("in use by another program");
            break;
        case common::Status::kInvalidArgument:
            explanation.reason = span_of("not a serial device");
            break;
        case common::Status::kNotSupported:
            explanation.reason = span_of("the device does not accept the line settings");
            break;
        case common::Status::kOk:
        case common::Status::kSystemError:
        case common::Status::kInterrupted:
        case common::Status::kDeadlineMissed:
        case common::Status::kAlreadyRunning:
        case common::Status::kNotRunning:
        case common::Status::kOverflow:
        case common::Status::kAlreadyExists:
        case common::Status::kOutOfRange:
        case common::Status::kTimeout:
        case common::Status::kDisconnected:
            explanation.reason = describe(status);
            break;
    }
    return explanation;
}

Explanation explain_open_failure(const common::Status status, const std::string_view device) noexcept {
    Explanation explanation{explain_open_failure(status)};
    const bool unreachable{(status == common::Status::kTimeout) || (status == common::Status::kDisconnected)};
    if (starts_with(device, kTcpPrefix)) {
        if (status == common::Status::kInvalidArgument) {
            explanation.reason = span_of("not an IP address and port");
            explanation.advice = "e.g. tcp:192.168.0.10:35000";
        } else if (unreachable) {
            explanation.reason = span_of("nothing answers at that address");
            explanation.advice = "join the adapter's Wi-Fi network first";
        } else {
            // As for a serial device.
        }
    } else if (starts_with(device, kBluetoothPrefix) || starts_with(device, kBluetoothLowEnergyPrefix)) {
        if (status == common::Status::kNotFound) {
            explanation.advice = "pair the adapter in the Bluetooth settings first";
        } else if (status == common::Status::kPermissionDenied) {
#if defined(__APPLE__)
            explanation.advice = "allow the app to use Bluetooth in the Settings";
#else
            explanation.advice = "allow the app to connect to nearby devices";
#endif
        } else if (status == common::Status::kNotRunning) {
            explanation.reason = span_of("Bluetooth is off");
            explanation.advice = "turn Bluetooth on";
        } else if (unreachable) {
            explanation.reason = span_of("the adapter does not accept the connection");
            explanation.advice = "is it plugged in, and no other device connected to it?";
        } else {
            // As for a serial device.
        }
    } else {
        // A serial device.
    }
    return explanation;
}

Explanation explain_connect_failure(const ConnectFailure failure, const common::Status status) noexcept {
    return explain_connect_failure(failure, status, std::string_view{});
}

Explanation explain_connect_failure(const ConnectFailure failure, const common::Status status,
                                    const std::string_view device) noexcept {
    Explanation explanation{};
    if (failure == ConnectFailure::kOpen) {
        explanation = explain_open_failure(status, device);
    } else if (failure == ConnectFailure::kNoAnswer) {
        explanation.reason = span_of("the adapter does not answer");
        explanation.advice = "check the device and the baud rate";
    } else if ((failure == ConnectFailure::kInitialize) && (status == common::Status::kNotSupported)) {
        explanation.reason = span_of("the adapter rejected its configuration");
    } else {
        explanation.reason = describe(status);  // "interrupted" for kInterrupted as well.
    }
    return explanation;
}

}  // namespace elm327
