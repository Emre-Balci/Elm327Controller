#include "elm327/controller/event.hpp"

#if COMMON_LINUX_API
#include <sys/eventfd.h>
#else
#include <fcntl.h>
#endif
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace elm327 {
namespace {

#if !COMMON_LINUX_API
/// The byte that signal() writes into the pipe; take_stop_request() takes it for SIGINT.
constexpr std::uint8_t kSignalByte{0U};

/// Reads of clear(), and the bytes each takes: far more than signal() writes between two clears.
constexpr std::uint32_t kMaxClearReads{64U};
constexpr std::size_t kClearSize{64U};

/// Makes an end of the pipe non-blocking (a full pipe is readable anyway) and closed on exec.
/// @return true if both worked.
bool configure(const int descriptor) noexcept {
    const int flags{::fcntl(descriptor, F_GETFL)};
    return (flags >= 0) && (::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0) &&
           (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0);
}
#endif

}  // namespace

Event::~Event() {
    close();
}

common::Status Event::open() noexcept {
    common::Status status{is_open() ? common::Status::kInvalidArgument : common::Status::kOk};
    if (is_ok(status)) {
#if COMMON_LINUX_API
        const int flags{
            static_cast<int>(static_cast<std::uint32_t>(EFD_CLOEXEC) | static_cast<std::uint32_t>(EFD_NONBLOCK))};
        descriptor_ = ::eventfd(0U, flags);
        status = (descriptor_ >= 0) ? common::Status::kOk : common::Status::kResourceLimit;
#else
        // No eventfd: a pipe, whose read end is the descriptor.
        std::array<int, 2U> ends{{-1, -1}};
        const bool created{::pipe(ends.data()) == 0};
        if (created && configure(ends[0U]) && configure(ends[1U])) {
            descriptor_ = ends[0U];
            write_end_ = ends[1U];
        } else {
            if (created) {
                static_cast<void>(::close(ends[0U]));
                static_cast<void>(::close(ends[1U]));
            }
            status = common::Status::kResourceLimit;
        }
#endif
    }
    return status;
}

void Event::close() noexcept {
    if (descriptor_ != common::kInvalidNativeHandle) {
        static_cast<void>(::close(descriptor_));
        descriptor_ = common::kInvalidNativeHandle;
    }
#if !COMMON_LINUX_API
    if (write_end_ != common::kInvalidNativeHandle) {
        static_cast<void>(::close(write_end_));
        write_end_ = common::kInvalidNativeHandle;
    }
#endif
}

void Event::signal() const noexcept {
#if COMMON_LINUX_API
    const std::uint64_t one{1U};
    const ssize_t count{::write(descriptor_, &one, sizeof(one))};
#else
    const ssize_t count{::write(write_end_, &kSignalByte, sizeof(kSignalByte))};
#endif
    // Fails only when the counter would overflow, or the pipe is full: readable then anyway.
    static_cast<void>(count);
}

void Event::clear() const noexcept {
#if COMMON_LINUX_API
    std::uint64_t value{0U};
    const ssize_t count{::read(descriptor_, &value, sizeof(value))};
    static_cast<void>(count);  // Nothing to clear is fine.
#else
    std::array<std::uint8_t, kClearSize> scratch{};
    bool more{true};
    for (std::uint32_t round{0U}; more && (round < kMaxClearReads); ++round) {
        more = ::read(descriptor_, scratch.data(), scratch.size()) == static_cast<ssize_t>(scratch.size());
    }
#endif
}

}  // namespace elm327
