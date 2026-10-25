#include "elm327/adapter/response.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/protocol/text.hpp"

namespace elm327 {
namespace {

/// A message the adapter prints as a line of its own.
struct KnownLine final {
    std::string_view text;
    Reply reply;
};

/// The fixed messages of the ELM327 (data sheet, "Messages and their meanings"). "BUS INIT",
/// "<DATA ERROR", "<RX ERROR" and "ERRnn" have variable parts and are matched in classify_line().
constexpr std::array<KnownLine, 16U> kKnownLines{{
    {"OK", Reply::kOk},
    {"?", Reply::kUnknownCommand},
    {"NO DATA", Reply::kNoData},
    {"SEARCHING...", Reply::kProgress},
    {"UNABLE TO CONNECT", Reply::kUnableToConnect},
    {"BUS ERROR", Reply::kBusError},
    {"BUS BUSY", Reply::kBusError},
    {"CAN ERROR", Reply::kBusError},
    {"FB ERROR", Reply::kBusError},
    {"DATA ERROR", Reply::kDataError},
    {"<RX ERROR", Reply::kDataError},
    {"BUFFER FULL", Reply::kBufferFull},
    {"STOPPED", Reply::kStopped},
    {"LV RESET", Reply::kAdapterError},
    {"ACT ALERT", Reply::kAdapterError},
    {"LP ALERT", Reply::kAdapterError},
}};

/// "ERR" and two digits: an internal error of the adapter, e.g. "ERR94" after a CAN bus fault.
bool is_internal_error(const common::Span<const char> text) noexcept {
    return (text.size() == 5U) && common::starts_with(text, "ERR") && common::is_decimal_digit(text[3U]) &&
           common::is_decimal_digit(text[4U]);
}

/// Order of the kinds that are not errors: kEmpty < kProgress < kOk < kContent.
std::uint32_t rank(const Reply reply) noexcept {
    std::uint32_t result{4U};  // The errors, which rank above all the others.
    switch (reply) {
        case Reply::kEmpty:
            result = 0U;
            break;
        case Reply::kProgress:
            result = 1U;
            break;
        case Reply::kOk:
            result = 2U;
            break;
        case Reply::kContent:
            result = 3U;
            break;
        case Reply::kUnknownCommand:
        case Reply::kNoData:
        case Reply::kUnableToConnect:
        case Reply::kBusError:
        case Reply::kDataError:
        case Reply::kBufferFull:
        case Reply::kStopped:
        case Reply::kAdapterError:
            break;
    }
    return result;
}

/// @return true if a line of kind @p candidate becomes the reply of a response whose reply is
///         @p current: the first error wins over everything, otherwise the higher rank.
bool supersedes(const Reply candidate, const Reply current) noexcept {
    return (!is_error(current)) && (is_error(candidate) || (rank(candidate) > rank(current)));
}

}  // namespace

bool is_error(const Reply reply) noexcept {
    bool error{true};
    switch (reply) {
        case Reply::kEmpty:
        case Reply::kProgress:
        case Reply::kOk:
        case Reply::kContent:
            error = false;
            break;
        case Reply::kUnknownCommand:
        case Reply::kNoData:
        case Reply::kUnableToConnect:
        case Reply::kBusError:
        case Reply::kDataError:
        case Reply::kBufferFull:
        case Reply::kStopped:
        case Reply::kAdapterError:
            break;
    }
    return error;
}

const char* to_string(const Reply reply) noexcept {
    const char* text{"unknown"};
    switch (reply) {
        case Reply::kEmpty:
            text = "empty reply";
            break;
        case Reply::kProgress:
            text = "progress message";
            break;
        case Reply::kOk:
            text = "OK";
            break;
        case Reply::kContent:
            text = "content";
            break;
        case Reply::kUnknownCommand:
            text = "unknown command";
            break;
        case Reply::kNoData:
            text = "no data";
            break;
        case Reply::kUnableToConnect:
            text = "unable to connect";
            break;
        case Reply::kBusError:
            text = "bus error";
            break;
        case Reply::kDataError:
            text = "data error";
            break;
        case Reply::kBufferFull:
            text = "buffer full";
            break;
        case Reply::kStopped:
            text = "stopped";
            break;
        case Reply::kAdapterError:
            text = "adapter error";
            break;
    }
    return text;
}

common::Status status_of(const Reply reply) noexcept {
    common::Status status{common::Status::kSystemError};
    switch (reply) {
        case Reply::kEmpty:
        case Reply::kProgress:
        case Reply::kOk:
        case Reply::kContent:
            status = common::Status::kOk;
            break;
        case Reply::kUnknownCommand:
            status = common::Status::kNotSupported;
            break;
        case Reply::kNoData:
            status = common::Status::kNotFound;
            break;
        case Reply::kUnableToConnect:
            status = common::Status::kDisconnected;
            break;
        case Reply::kStopped:
            status = common::Status::kInterrupted;
            break;
        case Reply::kBusError:
        case Reply::kDataError:
        case Reply::kBufferFull:
        case Reply::kAdapterError:
            break;
    }
    return status;
}

Reply classify_line(const common::Span<const char> line) noexcept {
    const common::Span<const char> text{common::trim(line)};
    Reply reply{Reply::kContent};
    if (text.empty()) {
        reply = Reply::kEmpty;
    } else if (common::starts_with(text, "BUS INIT")) {
        // "BUS INIT: ...OK" or "BUS INIT: ...ERROR", while the adapter wakes up an ISO 9141 or
        // ISO 14230 bus.
        reply = common::ends_with(text, "ERROR") ? Reply::kBusError : Reply::kProgress;
    } else if (common::contains(text, "<DATA ERROR") || common::contains(text, "<RX ERROR")) {
        reply = Reply::kDataError;  // A line of data the adapter marks as corrupt.
    } else if (is_internal_error(text)) {
        reply = Reply::kAdapterError;
    } else {
        bool found{false};
        for (std::size_t index{0U}; (!found) && (index < kKnownLines.size()); ++index) {
            found = common::equals(text, kKnownLines[index].text);
            if (found) {
                reply = kKnownLines[index].reply;
            }
        }
    }
    return reply;
}

void Response::clear() noexcept {
    lines_.clear();
    reply_ = Reply::kEmpty;
    reply_index_ = kMaxLines;
    truncated_ = false;
}

void Response::add_line(const common::Span<const char> line, const bool cut) noexcept {
    const common::Span<const char> text{common::trim(line)};
    if (!text.empty()) {
        const bool fits{text.size() <= kMaxLineLength};
        Line stored{};
        // Cannot fail: at most kMaxLineLength characters are appended.
        static_cast<void>(stored.append(fits ? text : text.first(kMaxLineLength)));
        const Reply kind{classify_line(stored.text())};
        const bool kept{lines_.push_back(stored)};
        if (supersedes(kind, reply_)) {
            reply_ = kind;
            reply_index_ = kept ? (lines_.size() - 1U) : kMaxLines;
        }
        truncated_ = truncated_ || cut || (!fits) || (!kept);
    }
}

common::Span<const char> Response::line(const std::size_t index) const noexcept {
    return lines_[index].text();
}

common::Span<const char> Response::reply_line() const noexcept {
    return (reply_index_ < lines_.size()) ? line(reply_index_) : common::Span<const char>{};
}

}  // namespace elm327
