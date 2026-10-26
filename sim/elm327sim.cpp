/// @file elm327sim.cpp
/// @brief elm327sim: a simulated ELM327 adapter on a pseudo-terminal (on Windows, a named pipe),
///        connected to a car that keeps accelerating and braking, for trying elm327ctl without
///        hardware.
///
/// Usage: elm327sim [--legacy] [--ignition-off] [--slow]
///   --legacy        the car speaks ISO 9141-2 instead of CAN
///   --ignition-off  the car does not answer ("UNABLE TO CONNECT")
///   --slow          resets and the protocol search take as long as on a real adapter
///
/// It prints the device to use with elm327ctl -d or elm327gui -d and runs until Ctrl+C. Like the
/// tests, this tool is outside the compliance scope of the library.

#if defined(_WIN32)
#include <atomic>
#include <csignal>
#include <thread>
#else
#include <pthread.h>
#include <signal.h>
#include <time.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "common/time/clock.hpp"
#include "sim/fake_elm327.hpp"

namespace {

using namespace std::chrono_literals;

std::vector<std::uint8_t> big_endian(const std::int64_t value, const std::size_t bytes) {
    std::vector<std::uint8_t> data(bytes, std::uint8_t{0U});
    for (std::size_t index{0U}; index < bytes; ++index) {
        const std::uint64_t shift{8U * (bytes - 1U - index)};
        data[index] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) >> shift) & 0xFFU);
    }
    return data;
}

/// Updates the car for @p elapsed time since the start: a 30 s cycle of accelerating from 0 to
/// 120 km/h and braking to a stop, while the engine warms up.
void drive(elm327::sim::FakeElm327& adapter, const common::Duration elapsed) {
    const std::int64_t ms{elapsed.count() / 1'000'000};
    const std::int64_t phase{ms % 30'000};
    const std::int64_t speed{(phase < 15'000) ? ((phase * 120) / 15'000) : (((30'000 - phase) * 120) / 15'000)};
    const std::int64_t rpm{800 + (speed * 25)};
    const std::int64_t coolant{std::min<std::int64_t>(20 + (ms / 2'000), 90)};
    const std::int64_t throttle{(phase < 15'000) ? 35 : 8};
    adapter.set_pid(0x04U, big_endian(((15 + (speed / 2)) * 255) / 100, 1U));  // Engine load.
    adapter.set_pid(0x05U, big_endian(coolant + 40, 1U));
    adapter.set_pid(0x0CU, big_endian(rpm * 4, 2U));
    adapter.set_pid(0x0DU, big_endian(speed, 1U));
    adapter.set_pid(0x10U, big_endian(rpm * 2, 2U));  // Mass air flow: rpm / 50 g/s.
    adapter.set_pid(0x11U, big_endian((throttle * 255) / 100, 1U));
    adapter.set_pid(0x1FU, big_endian(ms / 1'000, 2U));  // Run time.
    adapter.set_pid(0x5CU, big_endian(std::max<std::int64_t>(coolant - 5, 20) + 40, 1U));
}

#if defined(_WIN32)
/// Set by Ctrl+C or Ctrl+Break; the handler runs on a thread of the console.
std::atomic<bool> g_stop{false};

void request_stop(const int /*signal*/) {
    g_stop.store(true);
}
#endif

}  // namespace

int main(const int argc, const char* const argv[]) {
    elm327::sim::Vehicle vehicle{elm327::sim::example_vehicle()};
    elm327::sim::Behaviour behaviour{};
    for (int index{1}; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--legacy") {
            vehicle.can = false;
        } else if (argument == "--ignition-off") {
            vehicle.ignition_on = false;
        } else if (argument == "--slow") {
            behaviour.reset_delay = 800ms;
            behaviour.search_delay = 2s;
        } else {
            std::fprintf(stderr, "usage: elm327sim [--legacy] [--ignition-off] [--slow]\n");
            return EXIT_FAILURE;
        }
    }

#if defined(_WIN32)
    static_cast<void>(std::signal(SIGINT, &request_stop));
    static_cast<void>(std::signal(SIGBREAK, &request_stop));
#else
    // Blocked before the simulation thread starts, so that only sigtimedwait() below takes them.
    sigset_t signals{};
    static_cast<void>(sigemptyset(&signals));
    static_cast<void>(sigaddset(&signals, SIGINT));
    static_cast<void>(sigaddset(&signals, SIGTERM));
    static_cast<void>(pthread_sigmask(SIG_BLOCK, &signals, nullptr));
#endif

    elm327::sim::FakeElm327 adapter{vehicle, behaviour};
    if (!adapter.valid()) {
#if defined(_WIN32)
        std::fprintf(stderr, "elm327sim: cannot create a named pipe\n");
#else
        std::fprintf(stderr, "elm327sim: cannot create a pseudo-terminal\n");
#endif
        return EXIT_FAILURE;
    }
    std::printf("Simulated ELM327 on %s, %s car%s.\n", adapter.device_path().c_str(),
                vehicle.can ? "CAN (ISO 15765-4)" : "ISO 9141-2", vehicle.ignition_on ? "" : " with the ignition off");
    std::printf("Try: elm327ctl -d %s         (the interactive shell)\n", adapter.device_path().c_str());
    std::printf("     elm327ctl -d %s info    (one command)\n", adapter.device_path().c_str());
    std::printf("     elm327gui -d %s --connect\n", adapter.device_path().c_str());
    std::printf("Stop with Ctrl+C.\n");
    static_cast<void>(std::fflush(stdout));

    const common::TimePoint start{common::MonotonicClock::now()};
#if defined(_WIN32)
    while (!g_stop.load()) {
        std::this_thread::sleep_for(100ms);
        drive(adapter, common::MonotonicClock::now() - start);
    }
#else
    bool running{true};
    while (running) {
        const timespec period{0, 100'000'000};
        running = sigtimedwait(&signals, nullptr, &period) < 0;  // A timeout (or EINTR) goes on.
        drive(adapter, common::MonotonicClock::now() - start);
    }
#endif
    std::printf("Stopped.\n");
    return EXIT_SUCCESS;
}
