/// @file bridged_link.hpp
/// @brief Links to adapters that only another runtime reaches: Bluetooth adapters through Java on
///        Android, or through Core Bluetooth on iOS.
///
/// The runtime connects to the adapter, answers one status byte (a LinkAnswer) on its end of a socket
/// pair, and then copies the bytes between the adapter and that end in both directions, until either
/// side closes; closing one side ends the other. open_bridged_link() makes the pair, hands one end
/// to the runtime and waits for its answer. The other end is what a LinkOpener returns, for
/// Adapter::open_stream(). Not on Windows.

#ifndef ELM327_CONTROLLER_BRIDGED_LINK_HPP
#define ELM327_CONTROLLER_BRIDGED_LINK_HPP

#include <cstdint>

#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/time/deadline.hpp"

namespace elm327 {

#if !defined(_WIN32)
/// The status byte a runtime answers on its end of the link, before the adapter's bytes.
enum class LinkAnswer : std::uint8_t {
    kConnected = 0U,
    kNotFound = 1U,      ///< No such adapter, e.g. not paired.
    kNoPermission = 2U,  ///< The app may not use Bluetooth.
    kOff = 3U,           ///< Bluetooth is off.
    kRefused = 4U,       ///< The adapter did not accept the connection.
};

/// Hands @p end, one end of the socket pair, and the @p address of the adapter to the runtime, which
/// owns @p end from then on, also when it fails. It returns at once, without waiting for the
/// connection.
using LinkHandOver = void (*)(const char* address, common::NativeHandle end, void* context) noexcept;

/// Opens a link to the adapter at @p address through a runtime: makes a socket pair, gives one end to
/// @p hand_over, and waits for the runtime's answer at most until @p deadline, and until @p cancel is
/// readable (kInvalidNativeHandle for none). When it gives up, it closes its end, which tells the
/// runtime to drop the connection.
/// @return the other end of the pair, which the caller owns; kNotFound, kPermissionDenied, kNotRunning
///         (Bluetooth off) or kDisconnected (refused, or the runtime closed its end) for the answers;
///         kTimeout; kInterrupted; kResourceLimit if there is no socket pair.
[[nodiscard]] common::Result<common::NativeHandle> open_bridged_link(const char* address, LinkHandOver hand_over,
                                                                     void* context, const common::Deadline& deadline,
                                                                     common::NativeHandle cancel) noexcept;
#endif

}  // namespace elm327

#endif  // ELM327_CONTROLLER_BRIDGED_LINK_HPP
