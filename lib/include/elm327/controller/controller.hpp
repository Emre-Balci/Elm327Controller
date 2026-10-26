/// @file controller.hpp
/// @brief The connection to an ELM327 adapter and the vehicle behind it, as a front-end uses it.
///
/// A front-end, such as the elm327ctl shell or the elm327gui window, connects once and then runs
/// diagnostic operations over the same connection until it disconnects. Controller keeps the
/// Adapter and the ObdClient together with the settings they were opened with, and adds what every
/// front-end needs on top of them:
/// - connect() opens the device and initializes the adapter in one step, and connect_failure()
///   tells which step failed;
/// - the supported PIDs are read once per connection (ensure_supported_pids());
/// - an operation that finds the device gone (kDisconnected from the serial port, e.g. an unplugged
///   USB adapter) closes the connection, so connected() tells the front-end to connect again;
/// - PID values come back as PidReading values, which own their bytes and stay valid.
///
/// Like Adapter, a Controller blocks in serial I/O: use it on a thread that may wait, e.g. the main
/// thread of a command-line tool or the worker thread of a user interface, never on a real-time
/// thread.

#ifndef ELM327_CONTROLLER_CONTROLLER_HPP
#define ELM327_CONTROLLER_CONTROLLER_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/containers/static_vector.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/file/serial_port.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/obd/client.hpp"
#include "elm327/obd/dtc.hpp"
#include "elm327/obd/pid.hpp"
#include "elm327/obd/service.hpp"
#include "elm327/obd/vin.hpp"

namespace elm327 {

#if defined(_WIN32)
/// The usual COM port of the first USB serial adapter: COM1 and COM2 are kept for built-in ports.
inline constexpr std::string_view kDefaultDevice{"COM3"};
#else
/// The first USB serial adapter: FTDI, CH340 and PL2303 chips appear as /dev/ttyUSB<n>.
inline constexpr std::string_view kDefaultDevice{"/dev/ttyUSB0"};
#endif

/// Where and how Controller::connect() reaches the adapter.
struct ConnectionSettings final {
    /// Longest device path.
    static constexpr std::size_t kMaxDeviceLength{255U};

    /// kDefaultDevice at 38400 bit/s, automatic protocol and the default timeouts.
    ConnectionSettings() noexcept;

    /// Serial device, e.g. "/dev/ttyUSB0" (USB) or "/dev/rfcomm0" (Bluetooth); on Windows a COM
    /// port, e.g. "COM3".
    common::StaticString<kMaxDeviceLength> device{};
    common::BaudRate baud_rate{common::BaudRate::k38400};
    /// ELM327 protocol number: '0' (automatic), '1' to '9', 'A' to 'C' (see is_valid_protocol()).
    char protocol{'0'};
    Timeouts timeouts{};
};

/// The step at which Controller::connect() failed.
enum class ConnectFailure : std::uint8_t {
    kNone,        ///< connect() succeeded, or was not called.
    kOpen,        ///< The device could not be opened: see describe_open_failure().
    kNoAnswer,    ///< The adapter did not answer the reset: a wrong device or baud rate.
    kInitialize,  ///< The adapter answered, but rejected a setting or failed otherwise.
};

/// A line of text the adapter printed, e.g. its identity or "12.6V".
using AdapterText = common::StaticString<Response::kMaxLineLength>;

/// A service 01 PID as read from the vehicle.
struct PidReading final {
    /// Most data bytes kept; longer data are cut off. No service 01 PID of SAE J1979 sends more
    /// than 13.
    static constexpr std::size_t kMaxData{32U};

    std::uint8_t pid{0U};
    /// kOk when the vehicle answered; otherwise why not, see Controller::read_pid().
    common::Status status{common::Status::kNotFound};
    /// The data bytes after "41 <pid>".
    common::StaticVector<std::uint8_t, kMaxData> data{};
    /// The decoded value; a failure for a PID without a single value (kNotSupported), one that
    /// known_pids() does not list (kNotFound), or one that was not read.
    common::Result<Measurement> value{common::Result<Measurement>::failure(common::Status::kNotFound)};
};

/// Characters format_reading() writes at most.
inline constexpr std::size_t kMaxReadingText{3U * PidReading::kMaxData};

/// Writes the value of @p reading to the start of @p out: the measurement with the decimals of its
/// PID and, with @p with_unit, its unit, e.g. "1726.25 rpm"; or, for a PID without a single value
/// or one this library does not know, the data bytes in hexadecimal separated by @p separator
/// ('\0' for none), e.g. "82 07 E5 00". No NUL is written.
/// @return the number of characters written; kOutOfRange if @p out is too small (kMaxReadingText
///         always fits).
[[nodiscard]] common::Result<std::size_t> format_reading(const PidReading& reading, bool with_unit, char separator,
                                                         common::Span<char> out) noexcept;

/// An ELM327 adapter, connected or not, and the vehicle behind it.
///
/// The object embeds the Adapter, the ObdClient and their buffers (about 20 KiB) and never
/// allocates. Not thread-safe: one thread uses a Controller at a time.
class Controller final {
public:
    Controller() noexcept;
    ~Controller() = default;
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    Controller(Controller&&) = delete;
    Controller& operator=(Controller&&) = delete;

    /// Closes any earlier connection, opens settings.device and initializes the adapter (see
    /// Adapter::initialize()). On failure the device is closed again and connect_failure() tells
    /// which step failed.
    /// @return kOk; the errors of Adapter::open() (ConnectFailure::kOpen); kTimeout when the adapter
    ///         does not answer (ConnectFailure::kNoAnswer); otherwise the errors of
    ///         Adapter::initialize() (ConnectFailure::kInitialize).
    common::Status connect(const ConnectionSettings& settings) noexcept;

    /// Closes the connection, if there is one.
    void disconnect() noexcept;

    /// @return true between a successful connect() and disconnect(), or until an operation found
    ///         the device gone.
    [[nodiscard]] bool connected() const noexcept {
        return adapter_.is_open();
    }

    [[nodiscard]] ConnectFailure connect_failure() const noexcept {
        return failure_;
    }

    /// The settings of the last connect().
    [[nodiscard]] const ConnectionSettings& settings() const noexcept {
        return settings_;
    }

    /// What the adapter printed at the reset, e.g. "ELM327 v1.5"; empty when not connected.
    [[nodiscard]] common::Span<const char> identity() const noexcept {
        return adapter_.identity();
    }

    /// See Adapter::set_cancel_descriptor(): once @p descriptor is readable (on Windows, signaled),
    /// operations end with kInterrupted.
    void set_cancel_descriptor(const common::NativeHandle descriptor) noexcept {
        adapter_.set_cancel_descriptor(descriptor);
    }

    /// See Adapter::set_trace().
    void set_trace(TraceFunction function, void* context) noexcept {
        adapter_.set_trace(function, context);
    }

    /// Sets the access policy. In AccessPolicy::kReadOnly the Controller refuses, before sending
    /// anything, every operation that could change the vehicle: clear_dtcs(), and an execute() whose
    /// command is not a pure read (see command_access()). The reads (query(), the PIDs, the DTCs and
    /// the VIN) are unaffected. The policy can be changed at any time and takes effect on the next
    /// operation; it does not touch the connection.
    void set_access_policy(AccessPolicy policy) noexcept {
        policy_ = policy;
    }

    [[nodiscard]] AccessPolicy access_policy() const noexcept {
        return policy_;
    }

    /// @return true in AccessPolicy::kReadOnly, the read-only safety mode.
    [[nodiscard]] bool read_only() const noexcept {
        return policy_ == AccessPolicy::kReadOnly;
    }

    /// Sends an AT command that answers one line of text, e.g. "ATRV" ("12.6V"), "AT@1" (the
    /// description) or "ATDP" (the protocol), and returns the line in @p text.
    /// @return kOk; kNotSupported if the adapter does not know the command (it answers "?", as
    ///         clones do for the commands they leave out); otherwise the errors of
    ///         Adapter::execute().
    common::Status query(std::string_view command, AdapterText& text) noexcept;

    /// Makes sure the vehicle answers: the first OBD request after a reset lets an adapter in
    /// automatic mode find the vehicle's protocol (ObdClient::connect()). Nothing is sent when
    /// the supported PIDs are known already.
    /// @return kOk; kDisconnected when the adapter found no vehicle ("UNABLE TO CONNECT", e.g. with
    ///         the ignition off); otherwise the errors of ObdClient::connect().
    common::Status connect_vehicle() noexcept;

    /// Reads all ranges of supported PIDs again (ObdClient::read_supported_pids()).
    /// @return like connect_vehicle().
    common::Status read_supported_pids() noexcept;

    /// read_supported_pids() unless the PIDs are known already from this connection.
    common::Status ensure_supported_pids() noexcept;

    /// @return true once the supported PIDs of this connection are known.
    [[nodiscard]] bool knows_supported_pids() const noexcept {
        return knows_pids_;
    }

    /// The PIDs the vehicle supports, PID 00 included. All of them once knows_supported_pids();
    /// after connect_vehicle() only the first range (01 to 20).
    [[nodiscard]] const common::ByteSet& supported_pids() const noexcept {
        return obd_.supported_pids();
    }

    /// Reads the current value of service 01 PID @p pid. The status of the reading is kOk; kNotFound
    /// when the vehicle did not answer (e.g. a PID it does not support); kNotSupported when an ECU
    /// rejected the request; otherwise the errors of ObdClient::read_pid().
    [[nodiscard]] PidReading read_pid(std::uint8_t pid) noexcept;

    /// Reads the MIL status from PID 01. @return the status; the errors of read_pid().
    [[nodiscard]] common::Result<MilStatus> read_mil_status() noexcept;

    /// Reads the DTCs of @p kind. @return the results of ObdClient::read_dtcs().
    common::Status read_dtcs(DtcKind kind, DtcList& dtcs) noexcept;

    /// Clears the stored and pending DTCs and turns the MIL off (OBD-II service 04). This is the one
    /// write this library performs by itself, and it also resets freeze frames and readiness
    /// monitors, so a caller should confirm it with the user first.
    /// @return kPermissionDenied, without sending anything, in read-only mode (see
    ///         set_access_policy()); otherwise the results of ObdClient::clear_dtcs().
    common::Status clear_dtcs() noexcept;

    /// Reads the VIN. @return the results of ObdClient::read_vin().
    common::Status read_vin(Vin& vin) noexcept;

    /// Sends @p command as it is, an AT command or an OBD request in hexadecimal, and collects the
    /// reply in @p response, waiting as long as the protocol search may take (Timeouts::connect).
    /// After an AT command the supported PIDs are read again before they are needed, since the
    /// command may have reset the adapter or changed its protocol.
    ///
    /// In read-only mode a command that is not a pure read (command_access() is not Access::kRead,
    /// e.g. an OBD service 04 or a UDS write) is refused before anything is sent: @p response is
    /// cleared and the status is kPermissionDenied. AT commands and read requests are sent as usual.
    /// @return the results of Adapter::execute(); kPermissionDenied when the read-only gate refused
    ///         the command; the reply may be an error message of the adapter.
    common::Status execute(std::string_view command, Response& response) noexcept;

    /// The reply to the last query() or OBD operation, e.g. "NO DATA" after a failed read_pid():
    /// see explain_failure().
    [[nodiscard]] const Response& last_response() const noexcept {
        return query_last_ ? response_ : obd_.response();
    }

private:
    /// Closes the connection when @p status reports that the device is gone: kDisconnected without
    /// an error message of the adapter in @p response comes from the serial port. "UNABLE TO
    /// CONNECT" maps to kDisconnected as well, but means that the vehicle does not answer.
    /// @return @p status.
    common::Status checked(common::Status status, const Response& response) noexcept;

    Adapter adapter_{};
    ObdClient obd_;
    Response response_{};  ///< Replies to query().
    ConnectionSettings settings_{};
    ConnectFailure failure_{ConnectFailure::kNone};
    AccessPolicy policy_{AccessPolicy::kReadWrite};
    bool knows_pids_{false};
    bool query_last_{false};  ///< last_response() is response_ rather than the ObdClient's.
};

}  // namespace elm327

#endif  // ELM327_CONTROLLER_CONTROLLER_HPP
