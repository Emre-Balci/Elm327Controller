/// @file response.hpp
/// @brief What an ELM327 prints in reply to one command: its lines, and what each line means.
///
/// The adapter answers every command with zero or more lines and then prints its prompt ">". A line
/// is data from the vehicle ("41 0C 1A F8"), other text ("ELM327 v1.5", "12.6V"), an
/// acknowledgement ("OK"), a progress message ("SEARCHING...") or one of the adapter's error
/// messages ("NO DATA", "UNABLE TO CONNECT", "CAN ERROR", ...). classify_line() tells them apart,
/// and a Response keeps the lines of one reply together with the most significant of their kinds.

#ifndef ELM327_ADAPTER_RESPONSE_HPP
#define ELM327_ADAPTER_RESPONSE_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/containers/static_vector.hpp"
#include "common/core/status.hpp"

namespace elm327 {

/// What a line from the adapter means; for a whole Response, the most significant of its lines.
/// The kinds from kUnknownCommand on are errors (see is_error()).
enum class Reply : std::uint8_t {
    kEmpty,            ///< Nothing: a response without lines.
    kProgress,         ///< "SEARCHING...", "BUS INIT: ...OK": progress of a request, without content.
    kOk,               ///< "OK": the adapter accepted an AT command.
    kContent,          ///< Data or other text, e.g. "41 0C 1A F8", "ELM327 v1.5", "12.6V".
    kUnknownCommand,   ///< "?": the adapter did not understand the command.
    kNoData,           ///< "NO DATA": the vehicle did not answer, e.g. for a PID it does not support.
    kUnableToConnect,  ///< "UNABLE TO CONNECT": no vehicle bus found (ignition off, wrong protocol).
    kBusError,         ///< "BUS ERROR", "BUS BUSY", "CAN ERROR", "FB ERROR", "BUS INIT: ...ERROR".
    kDataError,        ///< "DATA ERROR", "<DATA ERROR", "<RX ERROR": a message arrived corrupted.
    kBufferFull,       ///< "BUFFER FULL": the adapter could not pass the messages on fast enough.
    kStopped,          ///< "STOPPED": a character sent to the adapter interrupted the command.
    kAdapterError,     ///< "LV RESET", "ERRnn", "ACT ALERT", "LP ALERT": the adapter failed or sleeps.
};

/// @return true for the kinds that report an error, kUnknownCommand to kAdapterError.
[[nodiscard]] bool is_error(Reply reply) noexcept;

/// @return a short description, e.g. "no data" or "unable to connect".
[[nodiscard]] const char* to_string(Reply reply) noexcept;

/// The Status an operation reports when the adapter replied with @p reply: kOk for the kinds that
/// are not errors, kNotFound for kNoData, kDisconnected for kUnableToConnect, kNotSupported for
/// kUnknownCommand, kInterrupted for kStopped, and kSystemError for the other errors.
[[nodiscard]] common::Status status_of(Reply reply) noexcept;

/// Classifies one line printed by the adapter, without its line end. White space around the text
/// is ignored, and an empty line is kEmpty.
[[nodiscard]] Reply classify_line(common::Span<const char> line) noexcept;

/// The lines of one reply of the adapter, without empty lines and without the echo of the command.
///
/// A Response holds at most kMaxLines lines of at most kMaxLineLength characters. Further lines
/// are dropped and a longer line keeps its first kMaxLineLength characters; both set truncated().
/// The object embeds its storage (about 4.6 KiB) and never allocates. Not thread-safe.
class Response final {
public:
    static constexpr std::size_t kMaxLines{32U};
    static constexpr std::size_t kMaxLineLength{128U};

    /// Removes all lines.
    void clear() noexcept;

    /// Adds @p line without the white space around it; an empty line is skipped. @p cut tells that
    /// the line was already shortened, e.g. by the framer that received it.
    void add_line(common::Span<const char> line, bool cut) noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return lines_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return lines_.empty();
    }

    /// @pre index < size()
    [[nodiscard]] common::Span<const char> line(std::size_t index) const noexcept;

    /// The most significant kind among the lines: the first error, otherwise kContent, kOk,
    /// kProgress or kEmpty, in this order.
    [[nodiscard]] Reply reply() const noexcept {
        return reply_;
    }

    /// The first line whose kind is reply(), e.g. "CAN ERROR" for kBusError; empty without lines.
    [[nodiscard]] common::Span<const char> reply_line() const noexcept;

    /// @return true if lines were dropped or shortened.
    [[nodiscard]] bool truncated() const noexcept {
        return truncated_;
    }

private:
    using Line = common::StaticString<kMaxLineLength>;

    common::StaticVector<Line, kMaxLines> lines_{};
    Reply reply_{Reply::kEmpty};
    std::size_t reply_index_{kMaxLines};  ///< Index of reply_line(); kMaxLines if it was dropped.
    bool truncated_{false};
};

}  // namespace elm327

#endif  // ELM327_ADAPTER_RESPONSE_HPP
