/// @file service.hpp
/// @brief Classifies a diagnostic request as reading or writing the vehicle, for the read-only mode.
///
/// The read-only safety mode must let a request reach the vehicle only when it cannot change
/// anything there. The decision is made on the service (the first byte of an OBD-II request, or the
/// service identifier of a UDS/KWP request), which is what says whether the request reads data or
/// acts on an ECU: clears codes, actuates an output, codes or programs a module, resets it, or
/// unlocks it for writing.
///
/// The classification is deliberately conservative. A service is kRead only when every protocol
/// that defines that byte treats it as a pure read; anything that writes in any of them is kWrite;
/// and a byte no protocol here defines is kUnknown. The read-only gate (command_blocked()) refuses
/// everything that is not kRead, so an unrecognized or ambiguous request is refused, never sent on a
/// guess. This "default-deny" is what makes the mode safe to trust: a request is allowed only when
/// it is known to be harmless, not merely because it is not known to be harmful.
///
/// These are free functions on a byte or a string; they do no I/O and need no adapter.

#ifndef ELM327_OBD_SERVICE_HPP
#define ELM327_OBD_SERVICE_HPP

#include <cstdint>
#include <string_view>

namespace elm327 {

/// What a diagnostic request does to the vehicle.
enum class Access : std::uint8_t {
    kRead,     ///< Only reads data, or configures the adapter; cannot change the vehicle.
    kWrite,    ///< May change the vehicle: clears codes, actuates an output, codes, programs, resets or unlocks.
    kUnknown,  ///< Not a request this library recognizes; the read-only gate treats it as a write.
};

/// Whether a Controller may change the vehicle, or only read from it.
enum class AccessPolicy : std::uint8_t {
    kReadWrite,  ///< Every command is allowed (the default).
    kReadOnly,   ///< Only commands that cannot change the vehicle are allowed; see command_access().
};

/// Classifies an OBD-II service (mode) of SAE J1979. The reads are 01 (current data), 02 (freeze
/// frame), 03 (stored DTCs), 05 (O2 monitor results), 06 (on-board monitor results), 07 (pending
/// DTCs), 09 (vehicle information) and 0A (permanent DTCs). The writes are 04 (clear the DTCs and
/// the MIL) and 08 (control of an on-board system, i.e. an actuator test). Anything else is
/// kUnknown.
[[nodiscard]] Access obd_service_access(std::uint8_t service) noexcept;

/// Classifies a UDS (ISO 14229) or KWP2000 (ISO 14230) service identifier, as a tester sends it
/// (the request side). The reads are the ones that only return data: ReadDataByIdentifier (22),
/// ReadMemoryByAddress (23), ReadScalingDataByIdentifier (24), ReadDataByPeriodicIdentifier (2A),
/// ReadDTCInformation (19), TesterPresent (3E), and the KWP read services 17, 18, 1A and 21.
/// Everything the standards define as changing an ECU is kWrite: session control (10), ECU reset
/// (11), clear diagnostic information (14), security access (27), communication control (28),
/// write/define/IO/routine services (2C, 2E, 2F, 30, 31, 3B, 3D), the upload/download block (34 to
/// 37), and the configuration services (83 to 87). A security-access request even to read a seed is
/// kWrite, because its only purpose is to unlock writing. Anything else is kUnknown.
[[nodiscard]] Access uds_service_access(std::uint8_t service) noexcept;

/// The safety-first classification of a service byte, whether it is read as OBD-II or as UDS/KWP:
/// kWrite if either reading writes, otherwise kRead if either reads, otherwise kUnknown. This is
/// what the gate uses, so a byte that is a read in one protocol and a write in another is treated as
/// a write and never mistaken for a safe read.
[[nodiscard]] Access service_access(std::uint8_t service) noexcept;

/// Classifies a whole command string as the adapter receives it:
/// - a command that begins with "AT" (in either case) is an adapter command (reset, echo, protocol,
///   header, voltage, ...): it configures the adapter and does not by itself put a request on the
///   vehicle's bus, so it is kRead;
/// - otherwise the string is an OBD/UDS request in hexadecimal, and its first byte is the service.
///   White space between or around the digits is ignored (the adapter ignores it too), so "0 4" is
///   still service 04. The first byte is classified with service_access().
/// A command that is empty, that is only white space, or whose first byte is not a pair of
/// hexadecimal digits (e.g. an "ST..." command of an STN adapter, which could transmit a frame) is
/// kUnknown, so the read-only gate refuses it.
[[nodiscard]] Access command_access(std::string_view command) noexcept;

/// @return true when @p command must be refused under @p policy: @p policy is kReadOnly and
///         command_access(command) is not kRead. This single predicate is the read-only gate; a
///         Controller in read-only mode calls it before every command it would send.
[[nodiscard]] bool command_blocked(std::string_view command, AccessPolicy policy) noexcept;

/// @return a short word for @p access: "read", "write" or "unknown".
[[nodiscard]] const char* to_string(Access access) noexcept;

}  // namespace elm327

#endif  // ELM327_OBD_SERVICE_HPP
