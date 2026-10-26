#include "elm327/controller/event.hpp"

#include <windows.h>

namespace elm327 {

Event::~Event() {
    close();
}

common::Status Event::open() noexcept {
    common::Status status{is_open() ? common::Status::kInvalidArgument : common::Status::kOk};
    if (is_ok(status)) {
        descriptor_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);  // Manual reset, not signaled.
        status = (descriptor_ != nullptr) ? common::Status::kOk : common::Status::kResourceLimit;
    }
    return status;
}

void Event::close() noexcept {
    if (descriptor_ != common::kInvalidNativeHandle) {
        static_cast<void>(::CloseHandle(descriptor_));
        descriptor_ = common::kInvalidNativeHandle;
    }
}

void Event::signal() const noexcept {
    static_cast<void>(::SetEvent(descriptor_));
}

void Event::clear() const noexcept {
    static_cast<void>(::ResetEvent(descriptor_));
}

}  // namespace elm327
