/// @file fake_elm327.hpp
/// @brief A simulated ELM327 adapter, connected to a simulated vehicle, on a pseudo-terminal.
///
/// FakeElm327 creates a pseudo-terminal and answers on its master side like an ELM327 v1.5:
/// AT commands (reset, echo, line feeds, spaces, protocols, voltage, ...) and OBD requests for the
/// service 01 PIDs, the trouble codes and the VIN, in the CAN (ISO 15765-4) form or the
/// ISO 9141-2 form. Programs open device_path() like the serial device of a real adapter.
///
/// It is the fixture of the tests and the heart of the elm327sim tool. Like the tests, it is
/// development tooling outside the compliance scope of the library: it uses std::string,
/// std::thread and the heap.

#ifndef ELM327_SIM_FAKE_ELM327_HPP
#define ELM327_SIM_FAKE_ELM327_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace elm327 {
namespace sim {

/// The vehicle behind the simulated adapter.
struct Vehicle final {
    /// With the ignition off, OBD requests fail with "UNABLE TO CONNECT".
    bool ignition_on{true};
    /// The vehicle's bus: ISO 15765-4 CAN 11 bit 500 kbit/s (protocol 6), or ISO 9141-2 (protocol 3).
    bool can{true};
    /// Data bytes of the service 01 PIDs, without "41 <pid>". The "supported PIDs" replies follow
    /// from them.
    std::map<std::uint8_t, std::vector<std::uint8_t>> pids{};
    std::vector<std::uint16_t> stored_dtcs{};
    std::vector<std::uint16_t> pending_dtcs{};
    std::vector<std::uint16_t> permanent_dtcs{};
    std::string vin{};
    /// The reply to ATRV.
    std::string voltage{"12.6V"};
};

/// A running car with typical values, two stored DTCs (P0133, P0420), one pending (P0171), one
/// permanent (P0420) and the VIN 1D4GP00R55B123456.
[[nodiscard]] Vehicle example_vehicle();

/// How the simulated adapter behaves.
struct Behaviour final {
    std::string identity{"ELM327 v1.5"};
    std::string description{"OBDII to RS232 Interpreter"};
    /// Line feeds after each "\r" until ATL0, as with pin 7 of a real ELM327 high.
    bool linefeeds{true};
    /// Time a reset (ATZ, ATWS) takes.
    std::chrono::milliseconds reset_delay{0};
    /// Time the protocol search of the first OBD request takes.
    std::chrono::milliseconds search_delay{0};
    /// The adapter never answers, like a device at another baud rate.
    bool silent{false};
};

class FakeElm327 final {
public:
    /// Creates the pseudo-terminal and starts answering on it.
    explicit FakeElm327(Vehicle vehicle = example_vehicle(), Behaviour behaviour = Behaviour{});

    /// Stops answering and removes the pseudo-terminal.
    ~FakeElm327();

    FakeElm327(const FakeElm327&) = delete;
    FakeElm327& operator=(const FakeElm327&) = delete;
    FakeElm327(FakeElm327&&) = delete;
    FakeElm327& operator=(FakeElm327&&) = delete;

    /// @return true if the pseudo-terminal exists.
    [[nodiscard]] bool valid() const;

    /// The device to open, e.g. "/dev/pts/3".
    [[nodiscard]] const std::string& device_path() const {
        return device_path_;
    }

    /// Sets the data of service 01 PID @p pid while the simulation runs.
    void set_pid(std::uint8_t pid, std::vector<std::uint8_t> data);

    /// The commands received so far, in upper case without spaces, e.g. "ATZ" or "010C".
    [[nodiscard]] std::vector<std::string> commands() const;

    /// Removes the pseudo-terminal: for the program that has it open, the device disappears.
    void hang_up();

private:
    void run();
    void receive(const char* data, std::size_t size);
    void process(const std::string& received);
    std::vector<std::string> at_command(const std::string& command);
    std::vector<std::string> obd_request(const std::string& command);
    std::vector<std::vector<std::uint8_t>> vehicle_messages(const std::vector<std::uint8_t>& request);
    std::vector<std::uint8_t> support_bitmap(std::uint8_t base) const;
    bool has_pid_above(std::uint32_t pid) const;
    std::vector<std::vector<std::uint8_t>> dtc_messages(std::uint8_t service,
                                                        const std::vector<std::uint16_t>& dtcs) const;
    std::vector<std::vector<std::uint8_t>> vin_messages() const;
    void add_lines(const std::vector<std::uint8_t>& message, std::vector<std::string>& lines) const;
    std::string format_bytes(const std::vector<std::uint8_t>& bytes) const;
    std::string describe_protocol() const;
    std::string protocol_number() const;
    char vehicle_protocol() const;
    void reset();
    void send(const std::string& text) const;

    mutable std::mutex mutex_;  ///< Guards vehicle_ and commands_.
    Vehicle vehicle_;
    std::vector<std::string> commands_{};
    const Behaviour behaviour_;

    // Adapter state, only used by the simulation thread.
    bool echo_{true};
    bool linefeeds_;
    bool spaces_{true};
    char protocol_{'0'};  ///< Selected with ATSP; kept across resets like in the EEPROM.
    bool connected_{false};
    std::string last_command_{};
    std::string input_{};

    int master_{-1};
    std::string device_path_{};
    std::atomic<bool> stop_{false};
    std::thread thread_{};
};

}  // namespace sim
}  // namespace elm327

#endif  // ELM327_SIM_FAKE_ELM327_HPP
