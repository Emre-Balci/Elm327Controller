/// @file vin.hpp
/// @brief The vehicle identification number (VIN) from service 09 PID 02 of SAE J1979.
///
/// On CAN (ISO 15765-4) the VIN arrives as one multi-frame message: "49 02 01" (01 is the number of
/// data items) and the 17 characters. The older protocols send five messages "49 02 <n>" with
/// n = 1 to 5 and four bytes each; the first starts with three padding bytes 00.

#ifndef ELM327_OBD_VIN_HPP
#define ELM327_OBD_VIN_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/static_string.hpp"
#include "common/core/status.hpp"
#include "elm327/obd/message.hpp"

namespace elm327 {

/// Characters of a VIN (ISO 3779).
inline constexpr std::size_t kVinLength{17U};

using Vin = common::StaticString<kVinLength>;

/// Assembles the VIN from @p messages, the reply to the request 09 02, in either form. Leading
/// padding bytes 00 are skipped. Only digits and capital letters are accepted.
/// @return kOk; kNotSupported if an ECU rejected the request (negative response); kInvalidArgument
///         if no message answers it, or the result is not 17 valid characters.
[[nodiscard]] common::Status decode_vin(const MessageList& messages, Vin& vin) noexcept;

}  // namespace elm327

#endif  // ELM327_OBD_VIN_HPP
