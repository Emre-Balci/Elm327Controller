/// @file demo.hpp
/// @brief A simulated car to look at: the car of elm327sim, which keeps accelerating and braking,
///        and the demo device of the Android app, which runs it on a socket pair.
///
/// Like the rest of the simulator, this is outside the compliance scope of the library: it uses
/// std::thread and the heap.

#ifndef ELM327_SIM_DEMO_HPP
#define ELM327_SIM_DEMO_HPP

#include "common/time/clock.hpp"
#include "sim/fake_elm327.hpp"

namespace elm327 {
namespace sim {

/// Updates the car of @p adapter for @p elapsed time since the start: a 30 s cycle of accelerating
/// from 0 to 120 km/h and braking to a stop, while the engine warms up.
void drive(FakeElm327& adapter, common::Duration elapsed);

#if !defined(_WIN32)
/// Starts a simulator with example_vehicle() that drive() moves every 100 ms, on a socket pair, in
/// place of the one before. Its identity is "ELM327 v1.5 (simulator)", so that nobody takes its
/// values for those of a car.
/// @return the other end of the socket pair, which the caller owns (elm327::Adapter::open_stream());
///         -1 if it cannot be created.
[[nodiscard]] int start_demo();

/// Stops the simulator of start_demo(), if it runs.
void stop_demo();
#endif

}  // namespace sim
}  // namespace elm327

#endif  // ELM327_SIM_DEMO_HPP
