#include "elm327/obd/client.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/static_string.hpp"
#include "common/protocol/text.hpp"
#include "elm327/obd/pid.hpp"

namespace elm327 {
namespace {

constexpr std::uint8_t kCurrentDataService{0x01U};

/// Ranges of supported PIDs: PID 00, 20, ..., E0, each listing the next 32 PIDs.
constexpr std::uint32_t kPidRanges{8U};
constexpr std::uint32_t kPidsPerRange{0x20U};

/// Longest request: a service, a PID and a few bytes more, in hexadecimal.
constexpr std::size_t kMaxRequestText{16U};

}  // namespace

common::Status ObdClient::request(const std::string_view command, const common::Deadline& deadline) noexcept {
    messages_.clear();
    common::Status status{adapter_->execute(command, response_, deadline)};
    last_reply_ = response_.reply();
    if (is_ok(status)) {
        if (is_error(last_reply_)) {
            status = status_of(last_reply_);
        } else if (last_reply_ != Reply::kContent) {
            status = common::Status::kInvalidArgument;  // E.g. "OK": not a reply to an OBD request.
        } else {
            status = parse_messages(response_, messages_);
        }
    }
    return status;
}

common::Status ObdClient::request_bytes(const common::Span<const std::uint8_t> bytes,
                                        const common::Deadline& deadline) noexcept {
    common::StaticString<kMaxRequestText> command{};
    const bool fits{common::append_hex_bytes(command, bytes, '\0')};
    return fits ? request(command.view(), deadline) : common::Status::kInvalidArgument;
}

common::Status ObdClient::connect() noexcept {
    supported_ = common::ByteSet{};
    bool more{false};
    return read_pid_range(0U, more);
}

common::Status ObdClient::read_supported_pids() noexcept {
    supported_ = common::ByteSet{};
    common::Status status{common::Status::kOk};
    bool more{true};
    for (std::uint32_t range{0U}; is_ok(status) && more && (range < kPidRanges); ++range) {
        status = read_pid_range(range, more);
        if ((range > 0U) && (status == common::Status::kNotFound)) {
            // A range that was announced but not answered ends the list; the PIDs so far are valid.
            status = common::Status::kOk;
            more = false;
        }
    }
    return status;
}

common::Status ObdClient::read_pid_range(const std::uint32_t range, bool& more) noexcept {
    const std::uint8_t base{static_cast<std::uint8_t>(range * kPidsPerRange)};
    const std::array<std::uint8_t, 2U> pid_request{{kCurrentDataService, base}};
    // The first request after a reset may make the adapter search for the protocol.
    const common::Duration timeout{(range == 0U) ? adapter_->timeouts().connect : adapter_->timeouts().command};
    common::Status status{request_bytes(pid_request, common::Deadline::after(timeout))};
    bool answered{false};
    bool rejected{false};
    // Every ECU that answers lists its own PIDs; the vehicle supports their union.
    for (std::size_t index{0U}; is_ok(status) && (index < messages_.size()); ++index) {
        const Message& message{messages_[index]};
        if (answers(message, pid_request)) {
            answered = true;
            status = decode_supported_pids(base, message.bytes().subspan(pid_request.size()), supported_);
        } else {
            rejected = rejected || rejects(message, kCurrentDataService);
        }
    }
    if (is_ok(status) && (!answered)) {
        status = rejected ? common::Status::kNotSupported : common::Status::kInvalidArgument;
    }
    if (is_ok(status) && (range == 0U)) {
        supported_ = supported_.with(base);  // PID 00 itself is always supported.
    }
    const std::uint32_t next{(range + 1U) * kPidsPerRange};
    more = is_ok(status) && (next <= 0xFFU) && supported_.contains(static_cast<std::uint8_t>(next));
    return status;
}

common::Result<common::Span<const std::uint8_t>> ObdClient::read_pid(const std::uint8_t pid) noexcept {
    using SpanResult = common::Result<common::Span<const std::uint8_t>>;
    const std::array<std::uint8_t, 2U> pid_request{{kCurrentDataService, pid}};
    const common::Status status{request_bytes(pid_request, common::Deadline::after(adapter_->timeouts().command))};
    SpanResult result{SpanResult::failure(common::Status::kInvalidArgument)};
    if (is_ok(status)) {
        const SpanResult response{find_response(messages_, pid_request)};
        result = response.has_value() ? SpanResult::success(response.value().subspan(pid_request.size())) : response;
    } else {
        result = SpanResult::failure(status);
    }
    return result;
}

common::Status ObdClient::read_dtcs(const DtcKind kind, DtcList& dtcs) noexcept {
    dtcs.clear();
    const std::array<std::uint8_t, 1U> dtc_request{{request_service(kind)}};
    common::Status status{request_bytes(dtc_request, common::Deadline::after(adapter_->timeouts().command))};
    bool answered{false};
    bool rejected{false};
    for (std::size_t index{0U}; is_ok(status) && (index < messages_.size()); ++index) {
        const Message& message{messages_[index]};
        if (answers(message, dtc_request)) {
            answered = true;
            status = decode_dtcs(message.bytes(), dtc_request[0U], dtcs);
        } else {
            rejected = rejected || rejects(message, dtc_request[0U]);
        }
    }
    if (is_ok(status) && (!answered)) {
        status = rejected ? common::Status::kNotSupported : common::Status::kInvalidArgument;
    }
    return status;
}

common::Status ObdClient::clear_dtcs() noexcept {
    const std::array<std::uint8_t, 1U> clear_request{{0x04U}};
    common::Status status{request_bytes(clear_request, common::Deadline::after(adapter_->timeouts().command))};
    bool answered{false};
    bool rejected{false};
    for (std::size_t index{0U}; is_ok(status) && (index < messages_.size()); ++index) {
        const Message& message{messages_[index]};
        if (answers(message, clear_request)) {
            answered = true;
        } else {
            rejected = rejected || rejects(message, clear_request[0U]);
        }
    }
    if (is_ok(status) && (!answered)) {
        status = rejected ? common::Status::kNotSupported : common::Status::kInvalidArgument;
    }
    return status;
}

common::Status ObdClient::read_vin(Vin& vin) noexcept {
    vin.clear();
    const std::array<std::uint8_t, 2U> vin_request{{0x09U, 0x02U}};
    common::Status status{request_bytes(vin_request, common::Deadline::after(adapter_->timeouts().command))};
    if (is_ok(status)) {
        status = decode_vin(messages_, vin);
    }
    return status;
}

}  // namespace elm327
