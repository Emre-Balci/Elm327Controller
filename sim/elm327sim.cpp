/// @file elm327sim.cpp
/// @brief elm327sim: a simulated ELM327 adapter on a pseudo-terminal (on Windows, a named pipe),
///        connected to a car that keeps accelerating and braking, for trying elm327ctl without
///        hardware.
///
/// Usage: elm327sim [--legacy] [--ignition-off] [--slow] [--tcp]
///   --legacy        the car speaks ISO 9141-2 instead of CAN
///   --ignition-off  the car does not answer ("UNABLE TO CONNECT")
///   --slow          resets and the protocol search take as long as on a real adapter
///   --tcp           a TCP port on 127.0.0.1, like an adapter on Wi-Fi (not on Windows)
///
/// It prints the device to use with elm327ctl -d or elm327gui -d and runs until Ctrl+C. Like the
/// tests, this tool is outside the compliance scope of the library.

#include "common/core/config.hpp"

#if defined(_WIN32) || !COMMON_LINUX_API
#include <atomic>
#include <csignal>
#include <thread>
#else
#include <pthread.h>
#include <signal.h>
#include <time.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "common/time/clock.hpp"
#include "sim/demo.hpp"
#include "sim/fake_elm327.hpp"

namespace {

using namespace std::chrono_literals;

#if defined(_WIN32) || !COMMON_LINUX_API
/// Set by Ctrl+C or Ctrl+Break (on Windows, where the handler runs on a thread of the console) or
/// SIGTERM (on macOS, which has no sigtimedwait()).
std::atomic<bool> g_stop{false};

void request_stop(const int /*signal*/) {
    g_stop.store(true);
}
#endif

}  // namespace

int main(const int argc, const char* const argv[]) {
    elm327::sim::Vehicle vehicle{elm327::sim::example_vehicle()};
    elm327::sim::Behaviour behaviour{};
    elm327::sim::Transport transport{elm327::sim::Transport::kDevice};
    for (int index{1}; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--legacy") {
            vehicle.can = false;
        } else if (argument == "--ignition-off") {
            vehicle.ignition_on = false;
        } else if (argument == "--slow") {
            behaviour.reset_delay = 800ms;
            behaviour.search_delay = 2s;
#if !defined(_WIN32)
        } else if (argument == "--tcp") {
            transport = elm327::sim::Transport::kTcp;
#endif
        } else {
            std::fprintf(stderr, "usage: elm327sim [--legacy] [--ignition-off] [--slow] [--tcp]\n");
            return EXIT_FAILURE;
        }
    }

#if defined(_WIN32)
    static_cast<void>(std::signal(SIGINT, &request_stop));
    static_cast<void>(std::signal(SIGBREAK, &request_stop));
#elif !COMMON_LINUX_API
    static_cast<void>(std::signal(SIGINT, &request_stop));
    static_cast<void>(std::signal(SIGTERM, &request_stop));
#else
    // Blocked before the simulation thread starts, so that only sigtimedwait() below takes them.
    sigset_t signals{};
    static_cast<void>(sigemptyset(&signals));
    static_cast<void>(sigaddset(&signals, SIGINT));
    static_cast<void>(sigaddset(&signals, SIGTERM));
    static_cast<void>(pthread_sigmask(SIG_BLOCK, &signals, nullptr));
#endif

    elm327::sim::FakeElm327 adapter{vehicle, behaviour, transport};
    if (!adapter.valid()) {
#if defined(_WIN32)
        std::fprintf(stderr, "elm327sim: cannot create a named pipe\n");
#else
        std::fprintf(stderr, "elm327sim: cannot create a pseudo-terminal or a TCP port\n");
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
#if defined(_WIN32) || !COMMON_LINUX_API
    while (!g_stop.load()) {
        std::this_thread::sleep_for(100ms);
        elm327::sim::drive(adapter, common::MonotonicClock::now() - start);
    }
#else
    bool running{true};
    while (running) {
        const timespec period{0, 100'000'000};
        running = sigtimedwait(&signals, nullptr, &period) < 0;  // A timeout (or EINTR) goes on.
        elm327::sim::drive(adapter, common::MonotonicClock::now() - start);
    }
#endif
    std::printf("Stopped.\n");
    return EXIT_SUCCESS;
}
