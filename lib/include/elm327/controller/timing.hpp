/// @file timing.hpp
/// @brief Periodic reads: a drift-free grid of sample times, and a wait for the next one that can
///        be cut short.
///
/// A monitor reads values every interval. A read takes as long as the vehicle needs, so the next
/// sample time is computed on the grid start + k * interval, never as "now + interval", and the
/// wait until it watches a descriptor (Ctrl+C, a new request of a user interface) like every
/// Adapter operation does.

#ifndef ELM327_CONTROLLER_TIMING_HPP
#define ELM327_CONTROLLER_TIMING_HPP

#include <cstdint>

#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"

namespace elm327 {

/// The activation after @p previous on the grid previous + k * interval that has not passed at
/// @p now: activations missed during slow reads are skipped instead of caught up in a burst.
/// @pre interval > 0
[[nodiscard]] common::TimePoint next_activation(common::TimePoint previous, common::Duration interval,
                                                common::TimePoint now) noexcept;

/// Waits until @p deadline, or until @p descriptor becomes readable; -1 waits for the deadline only.
/// @return kOk when the deadline passed; kInterrupted when the descriptor became readable first;
///         kSystemError if waiting failed.
common::Status wait_until(std::int32_t descriptor, const common::Deadline& deadline) noexcept;

}  // namespace elm327

#endif  // ELM327_CONTROLLER_TIMING_HPP
