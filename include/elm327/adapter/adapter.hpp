/// @file adapter.hpp
/// @brief Connection to an ELM327-compatible OBD-II adapter over a serial port.
///
/// An ELM327 is a command interpreter between a serial line and the diagnostic buses of a vehicle.
/// It takes one command at a time, terminated by "\r": an AT command that configures the adapter
/// ("ATZ", "ATE0", "ATSP0"), or an OBD request in hexadecimal ("010C"), which it sends to the
/// vehicle. It answers with lines that end in "\r" and then prints its prompt ">", ready for the
/// next command.
///
/// Adapter sends a command and collects the reply up to the prompt (execute()), and puts the
/// adapter into a known state (initialize()). Every operation waits at most until a deadline and
/// can be cancelled through a descriptor, e.g. a signalfd that becomes readable on Ctrl+C.
///
/// Serial I/O blocks in the kernel, so nothing here is safe on the real-time path: use an Adapter
/// on a non-real-time thread, as common::SerialPort requires.

#ifndef ELM327_ADAPTER_ADAPTER_HPP
#define ELM327_ADAPTER_ADAPTER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/core/status.hpp"
#include "common/file/serial_port.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "elm327/adapter/response.hpp"

namespace elm327 {

/// What a TraceFunction is told about.
enum class TraceEvent : std::uint8_t {
    kCommand,  ///< A command sent to the adapter, without its "\r".
    kLine,     ///< A line received from the adapter, including echoes and progress messages.
};

/// Observer of the traffic with the adapter, e.g. to print it in a verbose mode. It runs on the
/// thread that uses the Adapter and must not use the Adapter itself.
using TraceFunction = void (*)(TraceEvent event, common::Span<const char> text, void* context) noexcept;

/// How long Adapter operations wait for the adapter.
struct Timeouts final {
    /// An AT command or an OBD request. When the vehicle does not answer a request, the adapter
    /// itself replies "NO DATA" after its own timeout (about 200 ms), so this only expires when the
    /// adapter does not answer at all.
    common::Duration command{std::chrono::seconds{5}};
    /// A reset (ATZ), which takes an ELM327 about one second.
    common::Duration reset{std::chrono::seconds{5}};
    /// The first OBD request after a reset. With automatic protocol selection the adapter first
    /// tries the protocols one after the other ("SEARCHING..."), which can take over ten seconds.
    common::Duration connect{std::chrono::seconds{30}};
};

/// Settings for Adapter::open().
struct AdapterOptions final {
    /// Line settings. ELM327 adapters default to 38400 bit/s (some to 9600 or 115200), 8N1. The
    /// port is opened for exclusive use, so no other program can interleave its commands.
    common::SerialOptions serial{};
    Timeouts timeouts{};
};

/// @return true if @p protocol is an ELM327 protocol number: '0' (automatic), '1' to '9', or 'A'
///         to 'C' (see the ATSP command in the ELM327 data sheet).
[[nodiscard]] bool is_valid_protocol(char protocol) noexcept;

/// An ELM327 adapter on a serial port.
///
/// The object embeds its buffers (about 5 KiB) and never allocates. Not thread-safe: one thread
/// uses an Adapter at a time.
class Adapter final {
public:
    /// Longest command execute() accepts.
    static constexpr std::size_t kMaxCommandLength{64U};

    Adapter() noexcept;
    ~Adapter() = default;
    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;
    Adapter(Adapter&&) = delete;
    Adapter& operator=(Adapter&&) = delete;

    /// Opens the serial device @p path, e.g. "/dev/ttyUSB0" (USB) or "/dev/rfcomm0" (Bluetooth).
    /// @return the errors of common::SerialPort::open(), e.g. kNotFound, kPermissionDenied (not in
    ///         the dialout group) or kResourceLimit (in use by another program).
    common::Status open(const char* path, const AdapterOptions& options) noexcept;

    /// Closes the serial device. @return kOk, or the error of common::SerialPort::close().
    common::Status close() noexcept;

    [[nodiscard]] bool is_open() const noexcept {
        return port_.is_open();
    }

    /// Makes operations cancellable: once @p descriptor is readable (e.g. a signalfd after Ctrl+C,
    /// or an eventfd), operations stop waiting and fail with kInterrupted. -1, the default, turns
    /// it off. The descriptor stays owned by the caller.
    void set_cancel_descriptor(std::int32_t descriptor) noexcept {
        cancel_descriptor_ = descriptor;
    }

    /// Installs @p function to observe the traffic; nullptr removes it.
    void set_trace(TraceFunction function, void* context) noexcept {
        trace_function_ = function;
        trace_context_ = context;
    }

    /// Sends @p command (without the terminating "\r") and collects the reply in @p response, until
    /// the adapter's prompt or @p deadline. Input left over from an earlier command is discarded
    /// first, and the echo of the command is not part of the reply. The reply may be an error
    /// message of the adapter, e.g. "NO DATA": see Response::reply().
    /// @return kOk when the prompt arrived; kInvalidArgument when the adapter is not open, or for a
    ///         command that is empty, longer than kMaxCommandLength or contains "\r"; kTimeout;
    ///         kInterrupted (cancelled); kOutOfRange if the reply did not fit in @p response (which
    ///         holds the lines that fit) or the adapter kept sending without a prompt; otherwise
    ///         the errors of common::SerialPort, e.g. kDisconnected.
    common::Status execute(std::string_view command, Response& response, const common::Deadline& deadline) noexcept;

    /// execute() with the command timeout of the AdapterOptions.
    common::Status execute(std::string_view command, Response& response) noexcept;

    /// Resets the adapter (ATZ) and configures it for OBD requests: echo off (ATE0), line feeds off
    /// (ATL0) and the OBD protocol @p protocol. ATSP stores the protocol in the adapter's EEPROM, so
    /// it is only sent when the adapter does not have @p protocol selected already. Headers and
    /// spaces keep the defaults of the reset (headers off, spaces on), which the OBD layer expects.
    /// @return kOk; kInvalidArgument for a @p protocol that is_valid_protocol() rejects;
    ///         kNotSupported if the adapter rejected a command; otherwise the errors of execute(),
    ///         e.g. kTimeout when nothing answers (a wrong device or baud rate).
    common::Status initialize(char protocol) noexcept;

    /// What the adapter printed at the reset, e.g. "ELM327 v1.5"; empty before initialize().
    [[nodiscard]] common::Span<const char> identity() const noexcept {
        return identity_.text();
    }

    [[nodiscard]] const Timeouts& timeouts() const noexcept {
        return timeouts_;
    }

private:
    /// Reads the reply to @p command up to the prompt; see execute().
    common::Status read_reply(std::string_view command, Response& response, const common::Deadline& deadline) noexcept;

    /// Feeds @p received to the framer and passes complete lines on. Bytes after the prompt are
    /// dropped. @return true when the prompt arrived.
    bool consume(std::string_view command, common::Span<const std::uint8_t> received, Response& response,
                 bool& first_line) noexcept;

    /// Waits until the port has input or reports an error, the cancel descriptor is readable, or
    /// @p deadline passes. @return kOk, kInterrupted or kTimeout.
    common::Status wait_for_input(const common::Deadline& deadline) const noexcept;

    /// ATZ, and keeps the identification it prints.
    common::Status reset() noexcept;

    /// Sends @p command and expects "OK". @return kNotSupported for any other reply.
    common::Status expect_ok(std::string_view command) noexcept;

    /// Selects @p protocol with ATSP unless the adapter has it selected already.
    common::Status select_protocol(char protocol) noexcept;

    void trace(TraceEvent event, common::Span<const char> text) const noexcept;

    common::SerialPort port_{};
    common::DelimitedFramer<Response::kMaxLineLength> framer_;
    Response scratch_{};  ///< Replies to the commands initialize() sends.
    Timeouts timeouts_{};
    common::StaticString<Response::kMaxLineLength> identity_{};
    TraceFunction trace_function_{nullptr};
    void* trace_context_{nullptr};
    std::int32_t cancel_descriptor_{-1};
};

}  // namespace elm327

#endif  // ELM327_ADAPTER_ADAPTER_HPP
