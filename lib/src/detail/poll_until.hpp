/// @file poll_until.hpp
/// @brief poll() with a Deadline, for the POSIX code of the library.

#ifndef ELM327_DETAIL_POLL_UNTIL_HPP
#define ELM327_DETAIL_POLL_UNTIL_HPP

#include <poll.h>
#include <time.h>

#include <cstdint>
#include <type_traits>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"

namespace elm327 {
namespace detail {

/// @p count as the nfds_t of poll(): the same type as std::size_t with glibc, a narrower one in
/// bionic and on macOS and iOS, which only those get a cast for.
template <typename Count>
constexpr nfds_t to_nfds(const Count count) noexcept {
    if constexpr (std::is_same<Count, nfds_t>::value) {
        return count;
    } else {
        return static_cast<nfds_t>(count);
    }
}

/// Waits like poll() until one of @p entries is ready, at most until @p deadline (no timeout for
/// Deadline::never()): ppoll() with the time that is left where Linux has it (COMMON_LINUX_API),
/// otherwise poll() with whole milliseconds, rounded up so that it never ends before the deadline.
/// @return what the call returns: the number of ready entries, 0 at the deadline, -1 with errno.
inline int poll_until(const common::Span<pollfd> entries, const common::Deadline& deadline) noexcept {
#if COMMON_LINUX_API
    timespec timeout{};
    const timespec* timeout_pointer{nullptr};
    if (!deadline.is_never()) {
        const std::int64_t ns{deadline.remaining().count()};
        timeout.tv_sec = ns / common::MonotonicClock::kNanosPerSecond;
        timeout.tv_nsec = ns % common::MonotonicClock::kNanosPerSecond;
        timeout_pointer = &timeout;
    }
    return ::ppoll(entries.data(), to_nfds(entries.size()), timeout_pointer, nullptr);
#else
    constexpr std::int64_t kNanosPerMillisecond{1'000'000};
    constexpr std::int64_t kMaxMilliseconds{0x7FFFFFFF};
    int milliseconds{-1};
    if (!deadline.is_never()) {
        const std::int64_t ns{deadline.remaining().count()};
        const std::int64_t rounded_up{(ns / kNanosPerMillisecond) + (((ns % kNanosPerMillisecond) != 0) ? 1 : 0)};
        milliseconds = static_cast<int>((rounded_up < kMaxMilliseconds) ? rounded_up : kMaxMilliseconds);
    }
    return ::poll(entries.data(), to_nfds(entries.size()), milliseconds);
#endif
}

}  // namespace detail
}  // namespace elm327

#endif  // ELM327_DETAIL_POLL_UNTIL_HPP
