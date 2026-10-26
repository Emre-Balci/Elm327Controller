#include "sim/demo.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace elm327 {
namespace sim {
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

#if !defined(_WIN32)
Behaviour demo_behaviour() {
    Behaviour behaviour{};
    behaviour.identity = "ELM327 v1.5 (simulator)";
    return behaviour;
}

/// The simulator of start_demo() and the thread that drives its car.
class Demo final {
public:
    Demo() : adapter_{example_vehicle(), demo_behaviour(), Transport::kSocket}, thread_{[this]() { run(); }} {}

    ~Demo() {
        stop_.store(true);
        thread_.join();
    }

    Demo(const Demo&) = delete;
    Demo& operator=(const Demo&) = delete;
    Demo(Demo&&) = delete;
    Demo& operator=(Demo&&) = delete;

    FakeElm327& adapter() {
        return adapter_;
    }

private:
    void run() {
        const common::TimePoint start{common::MonotonicClock::now()};
        while (!stop_.load()) {
            drive(adapter_, common::MonotonicClock::now() - start);
            std::this_thread::sleep_for(100ms);
        }
    }

    FakeElm327 adapter_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::mutex g_mutex;
std::unique_ptr<Demo> g_demo{};
#endif

}  // namespace

void drive(FakeElm327& adapter, const common::Duration elapsed) {
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

#if !defined(_WIN32)
int start_demo() {
    const std::lock_guard<std::mutex> lock{g_mutex};
    g_demo.reset();  // The adapter's end of the old one closes: a link to it finds it gone.
    g_demo = std::make_unique<Demo>();
    return g_demo->adapter().valid() ? g_demo->adapter().take_stream() : -1;
}

void stop_demo() {
    const std::lock_guard<std::mutex> lock{g_mutex};
    g_demo.reset();
}
#endif

}  // namespace sim
}  // namespace elm327
