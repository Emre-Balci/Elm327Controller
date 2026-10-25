/// @file client.hpp
/// @brief OBD-II diagnostic services of SAE J1979 through an ELM327 adapter.

#ifndef ELM327_OBD_CLIENT_HPP
#define ELM327_OBD_CLIENT_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "common/time/deadline.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/obd/dtc.hpp"
#include "elm327/obd/message.hpp"
#include "elm327/obd/vin.hpp"

namespace elm327 {

/// Requests OBD-II services from the vehicle through an initialized Adapter: the supported PIDs
/// and their current values (service 01), the trouble codes (services 03, 07, 0A) and the VIN
/// (service 09).
///
/// A request waits for its reply until the command timeout of the adapter. The first request after
/// a reset may make the adapter search for the vehicle's protocol first, so connect() and
/// read_supported_pids() give it the longer connect timeout; call one of them first.
///
/// When a request fails because of the adapter's reply, last_reply() tells which one it was, e.g.
/// Reply::kNoData or Reply::kUnableToConnect, and response() holds its lines.
///
/// The object embeds the last reply and its messages (about 9 KiB) and never allocates. Not
/// thread-safe.
class ObdClient final {
public:
    /// @p adapter must be initialized and outlive the client.
    explicit ObdClient(Adapter& adapter) noexcept : adapter_{&adapter} {}

    /// Sends the OBD request @p command in hexadecimal, e.g. "010C" (service 01, PID 0C), waits for
    /// the reply until @p deadline and parses it into messages().
    /// @return kOk; for an error reply its status_of(), e.g. kNotFound for "NO DATA" or
    ///         kDisconnected for "UNABLE TO CONNECT"; kInvalidArgument for a reply that is not OBD
    ///         data; otherwise the errors of Adapter::execute() and parse_messages().
    common::Status request(std::string_view command, const common::Deadline& deadline) noexcept;

    /// Connects to the vehicle with its first request, PID 00, and records the first range of
    /// supported PIDs (01 to 20).
    /// @return kOk; kDisconnected when the adapter found no vehicle ("UNABLE TO CONNECT", e.g. with
    ///         the ignition off); otherwise the errors of request().
    common::Status connect() noexcept;

    /// Connects like connect() and reads all ranges of supported PIDs, as far as each range says
    /// that the next one exists. @return like connect().
    common::Status read_supported_pids() noexcept;

    /// The PIDs that connect() or read_supported_pids() found, including PID 00.
    [[nodiscard]] const common::ByteSet& supported_pids() const noexcept {
        return supported_;
    }

    /// Reads the current value of service 01 PID @p pid.
    /// @return the data bytes after "41 <pid>", valid until the next request; kNotFound when the
    ///         vehicle did not answer (e.g. a PID it does not support); kNotSupported when an ECU
    ///         rejected the request; otherwise the errors of request().
    [[nodiscard]] common::Result<common::Span<const std::uint8_t>> read_pid(std::uint8_t pid) noexcept;

    /// Reads the DTCs of @p kind from all ECUs that answer, without duplicates.
    /// @return kOk; kNotFound when no ECU answered ("NO DATA"; some ECUs do not answer when they
    ///         have no codes, or do not support the service); kNotSupported when an ECU rejected
    ///         the request; otherwise the errors of request() and decode_dtcs().
    common::Status read_dtcs(DtcKind kind, DtcList& dtcs) noexcept;

    /// Reads the vehicle identification number. @return kOk; otherwise the errors of request() and
    /// decode_vin().
    common::Status read_vin(Vin& vin) noexcept;

    /// The kind of the last reply; Reply::kEmpty if the adapter did not answer.
    [[nodiscard]] Reply last_reply() const noexcept {
        return last_reply_;
    }

    /// The lines of the last reply.
    [[nodiscard]] const Response& response() const noexcept {
        return response_;
    }

    /// The messages of the last reply.
    [[nodiscard]] const MessageList& messages() const noexcept {
        return messages_;
    }

private:
    /// request() for the request bytes @p bytes, e.g. {0x01, 0x0C}.
    common::Status request_bytes(common::Span<const std::uint8_t> bytes, const common::Deadline& deadline) noexcept;

    /// Requests the range @p range (0 for PID 00, 1 for PID 20, ...) of supported PIDs and adds them
    /// to supported_. @p more tells whether the vehicle announces the next range.
    common::Status read_pid_range(std::uint32_t range, bool& more) noexcept;

    Adapter* adapter_;
    Response response_{};
    MessageList messages_{};
    common::ByteSet supported_{};
    Reply last_reply_{Reply::kEmpty};
};

}  // namespace elm327

#endif  // ELM327_OBD_CLIENT_HPP
