#include "elm327/adapter/adapter.hpp"

#include <poll.h>
#include <time.h>

#include <array>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/protocol/text.hpp"

namespace elm327 {
namespace {

/// Bytes taken from the port per read.
constexpr std::size_t kReadSize{64U};

/// Reads per reply: a full Response even when its bytes arrive one at a time, plus as many
/// wake-ups without data. A device that keeps sending without a prompt ends the loop.
constexpr std::size_t kMaxReads{2U * Response::kMaxLines * (Response::kMaxLineLength + 2U)};

/// Attempts of the reset: a partial command left in the adapter's input buffer (e.g. by another
/// program) makes the first ATZ fail with "?", and the second one then works.
constexpr std::uint32_t kResetAttempts{2U};

/// How long settle() waits for the late prompt of an interrupted command before it stops the
/// adapter: a request the vehicle does not answer ends after about 200 ms ("NO DATA").
constexpr common::Duration kSettleWait{std::chrono::milliseconds{300}};

/// Any character stops a command the adapter is working on; an idle adapter ignores a space.
constexpr std::string_view kStopCharacter{" "};

/// The protocol numbers of the ELM327: 0 is automatic, 1 to 9 and A to C are fixed protocols. The
/// same in lower case at the same positions, and the names, also at the same positions.
constexpr std::string_view kProtocols{"0123456789ABC"};
constexpr std::string_view kLowerProtocols{"0123456789abc"};
constexpr std::array<const char*, 13U> kProtocolNames{{
    "automatic",
    "SAE J1850 PWM (41.6 kbaud)",
    "SAE J1850 VPW (10.4 kbaud)",
    "ISO 9141-2 (5 baud init)",
    "ISO 14230-4 KWP (5 baud init)",
    "ISO 14230-4 KWP (fast init)",
    "ISO 15765-4 CAN (11 bit ID, 500 kbaud)",
    "ISO 15765-4 CAN (29 bit ID, 500 kbaud)",
    "ISO 15765-4 CAN (11 bit ID, 250 kbaud)",
    "ISO 15765-4 CAN (29 bit ID, 250 kbaud)",
    "SAE J1939 CAN (29 bit ID, 250 kbaud)",
    "USER1 CAN (11 bit ID, 125 kbaud)",
    "USER2 CAN (11 bit ID, 50 kbaud)",
}};
static_assert(kProtocolNames.size() == kProtocols.size(), "one name per protocol");

/// Lines end with "\r", and the prompt ">" ends the reply.
common::ByteSet line_delimiters() noexcept {
    return common::ByteSet{}.with('\r').with('>');
}

/// Line feeds (with ATL1) and the NUL bytes some adapters send are dropped.
common::ByteSet ignored_bytes() noexcept {
    return common::ByteSet{}.with('\n').with('\0');
}

timespec to_relative_timespec(const common::Duration duration) noexcept {
    const std::int64_t ns{duration.count()};
    timespec ts{};
    ts.tv_sec = ns / common::MonotonicClock::kNanosPerSecond;
    ts.tv_nsec = ns % common::MonotonicClock::kNanosPerSecond;
    return ts;
}

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

}  // namespace

bool is_valid_protocol(const char protocol) noexcept {
    return kProtocols.find(protocol) != std::string_view::npos;
}

common::Result<char> parse_protocol(const common::Span<const char> text) noexcept {
    std::size_t position{std::string_view::npos};
    if (text.size() == 1U) {
        position = kProtocols.find(text[0U]);
        if (position == std::string_view::npos) {
            position = kLowerProtocols.find(text[0U]);
        }
    }
    return (position != std::string_view::npos) ? common::Result<char>::success(kProtocols[position])
                                                : common::Result<char>::failure(common::Status::kInvalidArgument);
}

const char* protocol_name(const char protocol) noexcept {
    const std::size_t position{kProtocols.find(protocol)};
    return (position != std::string_view::npos) ? kProtocolNames[position] : "unknown";
}

std::string_view protocols() noexcept {
    return kProtocols;
}

Adapter::Adapter() noexcept : framer_{line_delimiters(), ignored_bytes()} {}

common::Status Adapter::open(const char* const path, const AdapterOptions& options) noexcept {
    const common::Status status{port_.open(path, options.serial)};
    if (is_ok(status)) {
        timeouts_ = options.timeouts;
        identity_.clear();
        unsettled_ = false;
    }
    return status;
}

common::Status Adapter::close() noexcept {
    identity_.clear();
    unsettled_ = false;
    return port_.close();
}

common::Status Adapter::execute(const std::string_view command, Response& response,
                                const common::Deadline& deadline) noexcept {
    response.clear();
    common::Status status{port_.is_open() ? common::Status::kOk : common::Status::kInvalidArgument};
    common::StaticString<kMaxCommandLength + 1U> line{};
    if (is_ok(status)) {
        const bool valid{(!command.empty()) && (command.find('\r') == std::string_view::npos) &&
                         line.append(command) && line.append('\r')};
        status = valid ? common::Status::kOk : common::Status::kInvalidArgument;
    }
    if (is_ok(status) && unsettled_) {
        status = settle(response, deadline);
        response.clear();
    }
    if (is_ok(status)) {
        // Late output of an earlier command, e.g. one that timed out, would be taken for the reply.
        status = port_.discard_input();
    }
    if (is_ok(status)) {
        framer_.reset();
        trace(TraceEvent::kCommand, span_of(command));
        status = port_.write_all(line.bytes(), deadline);
        // From here on the adapter works on the command: without its prompt, the next command
        // has to settle first.
        unsettled_ = true;
    }
    if (is_ok(status)) {
        status = read_reply(command, response, deadline);
        unsettled_ = !is_ok(status);
    }
    if (is_ok(status) && response.truncated()) {
        status = common::Status::kOutOfRange;
    }
    return status;
}

common::Status Adapter::execute(const std::string_view command, Response& response) noexcept {
    return execute(command, response, common::Deadline::after(timeouts_.command));
}

common::Status Adapter::initialize(const char protocol) noexcept {
    common::Status status{is_valid_protocol(protocol) ? common::Status::kOk : common::Status::kInvalidArgument};
    if (is_ok(status)) {
        status = reset();
    }
    if (is_ok(status)) {
        status = expect_ok("ATE0");  // Echo off: replies hold only the answer.
    }
    if (is_ok(status)) {
        status = expect_ok("ATL0");  // Line feeds off: lines end with "\r" alone.
    }
    if (is_ok(status)) {
        status = select_protocol(protocol);
    }
    return status;
}

common::Status Adapter::read_reply(const std::string_view command, Response& response,
                                   const common::Deadline& deadline) noexcept {
    std::array<std::uint8_t, kReadSize> buffer{};
    common::Status status{common::Status::kOk};
    bool prompt{false};
    bool first_line{true};
    for (std::size_t round{0U}; is_ok(status) && (!prompt) && (round < kMaxReads); ++round) {
        status = wait_for_input(deadline);
        if (is_ok(status)) {
            // The port is ready, so an expired deadline still takes what has arrived.
            const common::Result<std::size_t> received{port_.read(buffer, common::Deadline{})};
            if (received.has_value()) {
                prompt = consume(command, common::Span<const std::uint8_t>{buffer.data(), received.value()}, response,
                                 first_line);
            } else if (received.status() != common::Status::kTimeout) {
                status = received.status();
            } else {
                // A wake-up without data: wait again.
            }
        }
    }
    if (is_ok(status) && (!prompt)) {
        status = common::Status::kOutOfRange;  // The adapter kept sending without a prompt.
    }
    return status;
}

common::Status Adapter::settle(Response& scratch, const common::Deadline& deadline) noexcept {
    // Usually the rest of the reply is on its way and arrives in a moment.
    const common::Deadline soon{common::Deadline::after(kSettleWait)};
    framer_.reset();
    common::Status status{read_reply(std::string_view{}, scratch,
                                     (soon.time_point() < deadline.time_point()) ? soon : deadline)};
    if ((status == common::Status::kTimeout) && (!deadline.expired())) {
        // Still busy, e.g. searching for the vehicle's protocol: stop it. The adapter answers
        // "STOPPED" and its prompt, or, if it finished meanwhile, ignores the space.
        common::StaticString<1U> stop{};
        static_cast<void>(stop.append(kStopCharacter));  // Cannot fail: one character.
        status = port_.write_all(stop.bytes(), deadline);
        if (is_ok(status)) {
            framer_.reset();
            status = read_reply(std::string_view{}, scratch, deadline);
        }
    }
    unsettled_ = !is_ok(status);
    return status;
}

bool Adapter::consume(const std::string_view command, const common::Span<const std::uint8_t> received,
                      Response& response, bool& first_line) noexcept {
    bool prompt{false};
    common::Span<const std::uint8_t> rest{received};
    // Each round consumes at least one byte.
    for (std::size_t round{0U}; (!rest.empty()) && (!prompt) && (round < kReadSize); ++round) {
        rest = rest.subspan(framer_.consume(rest));
        if (framer_.has_frame()) {
            const common::Span<const char> text{common::trim(framer_.text())};
            if (!text.empty()) {
                trace(TraceEvent::kLine, text);
                // With echo on (after a reset, until ATE0) the adapter first repeats the command.
                const bool echo{first_line && common::equals(text, command)};
                if (!echo) {
                    response.add_line(text, framer_.truncated());
                }
                first_line = false;
            }
            prompt = framer_.delimiter() == '>';
        }
    }
    return prompt;
}

common::Status Adapter::wait_for_input(const common::Deadline& deadline) const noexcept {
    std::array<pollfd, 2U> entries{};
    entries[0U].fd = port_.descriptor();
    entries[0U].events = POLLIN;
    entries[1U].fd = cancel_descriptor_;  // poll() skips a negative descriptor.
    entries[1U].events = POLLIN;
    timespec timeout{};
    const timespec* timeout_pointer{nullptr};
    if (!deadline.is_never()) {
        timeout = to_relative_timespec(deadline.remaining());
        timeout_pointer = &timeout;
    }
    const int ready{::ppoll(entries.data(), entries.size(), timeout_pointer, nullptr)};
    common::Status status{common::Status::kTimeout};
    if (ready < 0) {
        // After a signal handler ran, the read finds no data and the caller waits again.
        status = (errno == EINTR) ? common::Status::kOk : common::Status::kSystemError;
    } else if (ready == 0) {
        status = common::Status::kTimeout;
    } else if (static_cast<std::uint32_t>(entries[1U].revents) != 0U) {
        status = common::Status::kInterrupted;
    } else {
        status = common::Status::kOk;  // Input, a hang-up or an error: the read tells which.
    }
    return status;
}

common::Status Adapter::reset() noexcept {
    common::Status status{common::Status::kNotSupported};
    bool done{false};
    for (std::uint32_t attempt{0U}; (!done) && (attempt < kResetAttempts); ++attempt) {
        status = execute("ATZ", scratch_, common::Deadline::after(timeouts_.reset));
        if (is_ok(status) && (scratch_.reply() != Reply::kContent)) {
            status = common::Status::kNotSupported;  // E.g. "?": earlier input garbled the command.
        } else {
            done = true;  // The identification arrived, or the adapter does not answer at all.
        }
    }
    if (is_ok(status)) {
        // The identification is the last text line; some adapters print other bytes before it.
        for (std::size_t index{0U}; index < scratch_.size(); ++index) {
            if (classify_line(scratch_.line(index)) == Reply::kContent) {
                identity_.clear();
                // Cannot fail: a line has at most the capacity of identity_.
                static_cast<void>(identity_.append(scratch_.line(index)));
            }
        }
    }
    return status;
}

common::Status Adapter::expect_ok(const std::string_view command) noexcept {
    common::Status status{execute(command, scratch_)};
    if (is_ok(status) && (scratch_.reply() != Reply::kOk)) {
        status = common::Status::kNotSupported;
    }
    return status;
}

common::Status Adapter::select_protocol(const char protocol) noexcept {
    // ATDPN answers the selected protocol, e.g. "6", or "A6" when it is automatic ("A" and the
    // protocol it tries first). An adapter that does not know ATDPN still gets ATSP.
    common::Status status{execute("ATDPN", scratch_)};
    bool selected{false};
    if (is_ok(status) && (scratch_.reply() == Reply::kContent) && (scratch_.size() == 1U)) {
        const common::Span<const char> current{scratch_.line(0U)};
        if (protocol == '0') {
            selected = common::starts_with(current, "A") || common::equals(current, "0");
        } else {
            selected = (current.size() == 1U) && (current[0U] == protocol);
        }
    }
    if (is_ok(status) && (!selected)) {
        common::StaticString<5U> command{};
        // Cannot fail: "ATSP" and one character fit.
        static_cast<void>(command.append("ATSP") && command.append(protocol));
        status = expect_ok(command.view());
    }
    return status;
}

void Adapter::trace(const TraceEvent event, const common::Span<const char> text) const noexcept {
    if (trace_function_ != nullptr) {
        trace_function_(event, text, trace_context_);
    }
}

}  // namespace elm327
