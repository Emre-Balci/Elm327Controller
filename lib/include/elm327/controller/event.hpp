/// @file event.hpp
/// @brief A descriptor that one thread makes readable for another: to interrupt what an Adapter
///        waits for (its cancel descriptor), or to wake up a thread that waits for work.
///
/// It is an eventfd on Linux and Android, a manual-reset event on Windows, and a pipe on macOS and
/// iOS (and on Linux built with COMMON_PORTABLE_POSIX), whose read end is the descriptor and whose
/// write end signal() writes to.

#ifndef ELM327_CONTROLLER_EVENT_HPP
#define ELM327_CONTROLLER_EVENT_HPP

#include "common/core/config.hpp"
#include "common/core/status.hpp"

namespace elm327 {

/// An event descriptor, readable (on Windows, signaled) from signal() until clear().
class Event final {
public:
    Event() noexcept = default;
    /// Closes the descriptor.
    ~Event();
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    /// Creates the descriptor, not readable yet. @return kOk; kInvalidArgument if it is open
    ///         already; kResourceLimit if the system has no more descriptors.
    common::Status open() noexcept;

    /// Closes the descriptor, if it is open.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept {
        return descriptor_ != common::kInvalidNativeHandle;
    }

    /// The descriptor to wait for, e.g. with Adapter::set_cancel_descriptor() or
    /// elm327::wait_until(); kInvalidNativeHandle when closed.
    [[nodiscard]] common::NativeHandle descriptor() const noexcept {
        return descriptor_;
    }

    /// Makes the descriptor readable. Any thread may call it, also while another one waits.
    void signal() const noexcept;

    /// Makes the descriptor unreadable again.
    void clear() const noexcept;

private:
    common::NativeHandle descriptor_{common::kInvalidNativeHandle};
#if !defined(_WIN32) && !COMMON_LINUX_API
    common::NativeHandle write_end_{common::kInvalidNativeHandle};  ///< The pipe's other end.
#endif
};

}  // namespace elm327

#endif  // ELM327_CONTROLLER_EVENT_HPP
