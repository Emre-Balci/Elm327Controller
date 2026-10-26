/// @file tcp_link.hpp
/// @brief Connecting to an adapter on Wi-Fi ("tcp:<address>:<port>"), for Adapter::open().

#ifndef ELM327_DETAIL_TCP_LINK_HPP
#define ELM327_DETAIL_TCP_LINK_HPP

#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/time/deadline.hpp"

namespace elm327 {
namespace detail {

/// @return true if @p path is the device of an adapter on Wi-Fi (kTcpPrefix).
[[nodiscard]] bool is_tcp_device(const char* path) noexcept;

/// Connects to the adapter on Wi-Fi @p path, "tcp:<address>:<port>" with a numeric IPv4 address or
/// an IPv6 address in brackets, at most until @p deadline and until @p cancel is readable.
/// @return the connected socket, non-blocking; kInvalidArgument for another form of @p path;
///         kDisconnected when the connection is refused or the address is unreachable; kTimeout;
///         kInterrupted; kResourceLimit or kPermissionDenied from the system.
[[nodiscard]] common::Result<common::NativeHandle> connect_tcp(const char* path, const common::Deadline& deadline,
                                                               common::NativeHandle cancel) noexcept;

}  // namespace detail
}  // namespace elm327

#endif  // ELM327_DETAIL_TCP_LINK_HPP
