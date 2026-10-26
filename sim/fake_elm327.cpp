#include "sim/fake_elm327.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace elm327 {
namespace sim {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kHexDigits{"0123456789ABCDEF"};
constexpr std::string_view kProtocols{"0123456789ABC"};
constexpr std::string_view kCanProtocols{"6789ABC"};

/// CAN: the largest message in a single frame, and the data bytes in the first frame of a
/// multi-frame message and in each following one.
constexpr std::size_t kSingleFrameSize{7U};
constexpr std::size_t kFirstFrameSize{6U};
constexpr std::size_t kConsecutiveFrameSize{7U};

/// How often send() retries while the pseudo-terminal's buffer is full.
constexpr int kSendAttempts{100};

#if defined(_WIN32)
/// The buffers of the pipe, each way.
constexpr DWORD kPipeBuffer{4096U};

/// How long the simulation thread waits for I/O before it looks at stop_, in milliseconds.
constexpr DWORD kPollMilliseconds{20U};

/// How long send() waits for the program to take a reply off the pipe, in milliseconds.
constexpr DWORD kSendMilliseconds{1000U};

/// Numbers the pipes of the simulators of one process.
std::atomic<std::uint32_t> g_next_pipe{0U};

std::wstring widen(const std::string& text) {
    std::wstring wide(text.size() + 1U, L'\0');
    const int length{::MultiByteToWideChar(CP_UTF8, 0U, text.c_str(), -1, wide.data(), static_cast<int>(wide.size()))};
    wide.resize((length > 0) ? (static_cast<std::size_t>(length) - 1U) : 0U);
    return wide;
}
#endif

std::string hex_digit(const std::size_t value) {
    return std::string(1U, kHexDigits[value & 0x0FU]);
}

std::string hex_byte(const std::uint8_t value) {
    return hex_digit(static_cast<std::size_t>(value) >> 4U) + hex_digit(value);
}

/// Decodes pairs of upper-case hexadecimal digits. @return false for anything else.
bool decode_hex(const std::string& text, std::vector<std::uint8_t>& bytes) {
    bool valid{(text.size() % 2U) == 0U};
    for (std::size_t index{0U}; valid && (index < text.size()); index += 2U) {
        const std::size_t high{kHexDigits.find(text[index])};
        const std::size_t low{kHexDigits.find(text[index + 1U])};
        valid = (high != std::string_view::npos) && (low != std::string_view::npos);
        if (valid) {
            bytes.push_back(static_cast<std::uint8_t>((high << 4U) | low));
        }
    }
    return valid;
}

std::string protocol_name(const char protocol) {
    std::string name{"?"};
    switch (protocol) {
        case '1':
            name = "SAE J1850 PWM";
            break;
        case '2':
            name = "SAE J1850 VPW";
            break;
        case '3':
            name = "ISO 9141-2";
            break;
        case '4':
            name = "ISO 14230-4 (KWP 5BAUD)";
            break;
        case '5':
            name = "ISO 14230-4 (KWP FAST)";
            break;
        case '6':
            name = "ISO 15765-4 (CAN 11/500)";
            break;
        case '7':
            name = "ISO 15765-4 (CAN 29/500)";
            break;
        case '8':
            name = "ISO 15765-4 (CAN 11/250)";
            break;
        case '9':
            name = "ISO 15765-4 (CAN 29/250)";
            break;
        case 'A':
            name = "SAE J1939 (CAN 29/250)";
            break;
        case 'B':
            name = "USER1 (CAN 11/125)";
            break;
        case 'C':
            name = "USER2 (CAN 11/50)";
            break;
        default:
            break;
    }
    return name;
}

/// AT commands that only change settings the simulation does not model; they answer "OK".
bool is_accepted_setting(const std::string& name) {
    static const std::set<std::string> kSettings{"D", "H0", "H1", "M0", "M1", "AT0", "AT1", "AT2", "CAF0", "CAF1"};
    const bool timeout{(name.size() == 4U) && (name.compare(0U, 2U, "ST") == 0)};  // ATST hh.
    // The CAN addressing of expert mode and of the features: the header (ATSH, and ATCP for the
    // priority of 29-bit identifiers), the receive filter (ATCRA, or ATCRA alone to reset it), the
    // flow control (ATFCSH, ATFCSD, ATFCSM) and extended addressing (ATCEA). The header decides
    // which module answers (see Vehicle::modules); the others have no effect here.
    const std::array<std::string_view, 7U> kAddressing{{"SH", "CP", "CRA", "FCSH", "FCSD", "FCSM", "CEA"}};
    bool addressing{false};
    for (const std::string_view prefix : kAddressing) {
        addressing = addressing || (name.compare(0U, prefix.size(), prefix) == 0);
    }
    return timeout || addressing || (kSettings.count(name) > 0U);
}

std::vector<std::uint8_t> ascii(const std::string& text) {
    std::vector<std::uint8_t> bytes{};
    for (const char character : text) {
        bytes.push_back(static_cast<std::uint8_t>(character));
    }
    return bytes;
}

}  // namespace

Vehicle example_vehicle() {
    Vehicle vehicle{};
    vehicle.pids = {
        {0x01U, {0x82U, 0x07U, 0xE5U, 0x00U}},  // MIL on, 2 confirmed DTCs.
        {0x03U, {0x02U, 0x00U}},                // Fuel system in closed loop.
        {0x04U, {0x4DU}},                       // Engine load 30.2 %.
        {0x05U, {0x7BU}},                       // Coolant 83 degC.
        {0x06U, {0x80U}},                       // Short term fuel trim 0.0 %.
        {0x07U, {0x84U}},                       // Long term fuel trim 3.1 %.
        {0x0BU, {0x21U}},                       // Intake manifold 33 kPa.
        {0x0CU, {0x1AU, 0xF8U}},                // 1726 rpm.
        {0x0DU, {0x2AU}},                       // 42 km/h.
        {0x0EU, {0x90U}},                       // Timing advance 8.0 deg.
        {0x0FU, {0x3CU}},                       // Intake air 20 degC.
        {0x10U, {0x01U, 0xF4U}},                // Mass air flow 5.00 g/s.
        {0x11U, {0x33U}},                       // Throttle 20.0 %.
        {0x1CU, {0x01U}},                       // OBD-II as defined by the CARB.
        {0x1FU, {0x01U, 0x2CU}},                // Running for 300 s.
        {0x21U, {0x00U, 0x0CU}},                // 12 km with the MIL on.
        {0x2FU, {0xA1U}},                       // Fuel tank 63.1 %.
        {0x31U, {0x04U, 0xD2U}},                // 1234 km since the codes were cleared.
        {0x33U, {0x65U}},                       // Barometric pressure 101 kPa.
        {0x42U, {0x36U, 0xB0U}},                // Control module 14.000 V.
        {0x46U, {0x37U}},                       // Ambient air 15 degC.
        {0x5CU, {0x7EU}},                       // Oil 86 degC.
        {0xA6U, {0x00U, 0x01U, 0xE2U, 0x40U}},  // Odometer 12345.6 km.
    };
    vehicle.stored_dtcs = {0x0133U, 0x0420U};
    vehicle.pending_dtcs = {0x0171U};
    vehicle.permanent_dtcs = {0x0420U};
    vehicle.vin = "1D4GP00R55B123456";
    std::vector<std::uint8_t> vin_bytes{};
    for (const char character : vehicle.vin) {
        vin_bytes.push_back(static_cast<std::uint8_t>(character));
    }
    vehicle.data_by_identifier = {
        {std::uint16_t{0xF190U}, vin_bytes},  // VIN, read with 'uds read F190' (multi-frame on CAN).
        {0x0407U, {0x00U}},  // An example one-byte coding block, writable with 'uds write 0407 ..'.
    };
    // Two more modules, for the features: each tells its part number (F187) and keeps a coding
    // (0600) that only changes in the extended session; the cluster also has a locked setting.
    vehicle.modules = {
        {"714",  // Instrument cluster; replies from 77E.
         {
             {std::uint16_t{0xF187U}, ascii("SIM-CLUSTER-01")},
             {0x0600U, {0x12U, 0x00U, 0x40U, 0x00U}},  // Needle sweep off (byte 0 bit 0), digital speed on.
             {0x0601U, {0x00U}},                       // Needs security access.
         }},
        {"70E",  // Body control module; replies from 778.
         {
             {std::uint16_t{0xF187U}, ascii("SIM-BCM-02")},
             {0x0600U, {0x00U, 0x08U, 0x00U}},  // Mirrors fold off (byte 0 bit 1), daytime lights on.
             {0x0410U, {0x03U}},                // Comfort turn signal: 3 flashes.
         }},
    };
    vehicle.session_protected_dids = {0x0600U, 0x0410U};
    vehicle.locked_dids = {0x0601U};
    return vehicle;
}

#if defined(_WIN32)
FakeElm327::FakeElm327(Vehicle vehicle, Behaviour behaviour, const Transport transport)
    : vehicle_{std::move(vehicle)}, behaviour_{std::move(behaviour)}, linefeeds_{behaviour_.linefeeds} {
    if (transport != Transport::kDevice) {
        return;  // Windows has no socket pairs here: valid() is false.
    }
    const std::string name{"\\\\.\\pipe\\elm327sim-" + std::to_string(::GetCurrentProcessId()) + "-" +
                           std::to_string(g_next_pipe.fetch_add(1U))};
    // One instance, so that one program at a time has the simulated adapter, as with a serial port.
    const HANDLE pipe{::CreateNamedPipeW(widen(name).c_str(),
                                         PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                         1U, kPipeBuffer, kPipeBuffer, 0U, nullptr)};
    pipe_ = (pipe != INVALID_HANDLE_VALUE) ? pipe : nullptr;
    event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if ((pipe_ != nullptr) && (event_ != nullptr)) {
        device_path_ = name;
    }
    if (valid()) {
        thread_ = std::thread{[this]() { run(); }};
    }
}
#else
FakeElm327::FakeElm327(Vehicle vehicle, Behaviour behaviour, const Transport transport)
    : vehicle_{std::move(vehicle)},
      behaviour_{std::move(behaviour)},
      linefeeds_{behaviour_.linefeeds},
      socket_{transport == Transport::kSocket} {
    if (socket_) {
        std::array<int, 2U> ends{{-1, -1}};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends.data()) == 0) {
            master_ = ends[0U];
            stream_ = ends[1U];
            static_cast<void>(::fcntl(master_, F_SETFL, ::fcntl(master_, F_GETFL) | O_NONBLOCK));
            device_path_ = "socket";
        }
    } else {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    }
    if ((!socket_) && (master_ >= 0) && (::grantpt(master_) == 0) && (::unlockpt(master_) == 0)) {
        // Non-blocking: output nobody reads (the program closed the device) must not hang the thread.
        static_cast<void>(::fcntl(master_, F_SETFL, ::fcntl(master_, F_GETFL) | O_NONBLOCK));
        std::array<char, 128U> name{};
        if (::ptsname_r(master_, name.data(), name.size()) == 0) {
            device_path_ = name.data();
        }
    }
    if (valid()) {
        thread_ = std::thread{[this]() { run(); }};
    }
}
#endif

FakeElm327::~FakeElm327() {
    hang_up();
}

#if defined(_WIN32)
bool FakeElm327::valid() const {
    return (pipe_ != nullptr) && (event_ != nullptr) && (!device_path_.empty());
}
#else
bool FakeElm327::valid() const {
    return (master_ >= 0) && (!device_path_.empty());
}

int FakeElm327::take_stream() {
    const int stream{stream_};
    stream_ = -1;
    return stream;
}
#endif

void FakeElm327::set_pid(const std::uint8_t pid, std::vector<std::uint8_t> data) {
    const std::lock_guard<std::mutex> lock{mutex_};
    vehicle_.pids[pid] = std::move(data);
}

std::vector<std::string> FakeElm327::commands() const {
    const std::lock_guard<std::mutex> lock{mutex_};
    return commands_;
}

std::vector<std::uint8_t> FakeElm327::did_value(const std::string& header, const std::uint16_t did) const {
    const std::lock_guard<std::mutex> lock{mutex_};
    const std::map<std::uint16_t, std::vector<std::uint8_t>>* dids{&vehicle_.data_by_identifier};
    if (!header.empty()) {
        const auto module = vehicle_.modules.find(header);
        dids = (module != vehicle_.modules.end()) ? &module->second : nullptr;
    }
    std::vector<std::uint8_t> value{};
    if (dids != nullptr) {
        const auto entry = dids->find(did);
        if (entry != dids->end()) {
            value = entry->second;
        }
    }
    return value;
}

#if defined(_WIN32)
void FakeElm327::hang_up() {
    stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (pipe_ != nullptr) {
        // The program that has the pipe open finds it broken, like an unplugged adapter.
        static_cast<void>(::DisconnectNamedPipe(pipe_));
        static_cast<void>(::CloseHandle(pipe_));
        pipe_ = nullptr;
    }
    if (event_ != nullptr) {
        static_cast<void>(::CloseHandle(event_));
        event_ = nullptr;
    }
}

void FakeElm327::run() {
    std::array<char, 256U> buffer{};
    bool connected{false};
    while (!stop_.load()) {
        // Waits for a program to open the pipe, or for what it sends.
        OVERLAPPED overlapped{};
        overlapped.hEvent = event_;
        bool started{false};
        bool closed{false};  // The program has closed its end.
        if (connected) {
            started = (::ReadFile(pipe_, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped) !=
                       FALSE) ||
                      (::GetLastError() == ERROR_IO_PENDING);
            closed = !started;
        } else if (::ConnectNamedPipe(pipe_, &overlapped) != FALSE) {
            connected = true;
        } else {
            const DWORD error{::GetLastError()};
            if (error == ERROR_IO_PENDING) {
                started = true;
            } else if (error == ERROR_PIPE_CONNECTED) {
                connected = true;  // A program opened the pipe before the call.
            } else if (error == ERROR_NO_DATA) {
                static_cast<void>(::DisconnectNamedPipe(pipe_));  // One opened it and closed it again.
            } else {
                std::this_thread::sleep_for(10ms);  // Try again in a moment.
            }
        }
        if (started) {
            bool completed{false};
            while ((!completed) && (!stop_.load())) {
                completed = ::WaitForSingleObject(event_, kPollMilliseconds) == WAIT_OBJECT_0;
            }
            if (!completed) {
                static_cast<void>(::CancelIoEx(pipe_, &overlapped));  // Stopping.
            }
            DWORD count{0U};
            const bool succeeded{::GetOverlappedResult(pipe_, &overlapped, &count, TRUE) != FALSE};
            if (!completed) {
                // Stopping.
            } else if (!connected) {
                connected = succeeded;  // A program opened the pipe.
                if (!succeeded) {
                    static_cast<void>(::DisconnectNamedPipe(pipe_));
                }
            } else if (succeeded) {
                receive(buffer.data(), static_cast<std::size_t>(count));
            } else {
                closed = true;
            }
        }
        if (closed) {
            // Ready for the next program, as a pseudo-terminal can be opened again.
            static_cast<void>(::DisconnectNamedPipe(pipe_));
            connected = false;
        }
    }
}
#else
void FakeElm327::hang_up() {
    stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (master_ >= 0) {
        static_cast<void>(::close(master_));
        master_ = -1;
    }
    if (stream_ >= 0) {
        static_cast<void>(::close(stream_));  // Nobody took it.
        stream_ = -1;
    }
}

void FakeElm327::run() {
    std::array<char, 256U> buffer{};
    while (!stop_.load()) {
        pollfd entry{};
        entry.fd = master_;
        entry.events = POLLIN;
        const int ready{::poll(&entry, 1U, 20)};
        const bool readable{(ready > 0) && ((static_cast<unsigned>(entry.revents) & unsigned{POLLIN}) != 0U)};
        const ssize_t count{readable ? ::read(master_, buffer.data(), buffer.size()) : ssize_t{0}};
        if (count > 0) {
            receive(buffer.data(), static_cast<std::size_t>(count));
        } else if (ready > 0) {
            // No program has the device open: the master reports a hang-up until one opens it. (The
            // other end of a socket pair stays closed.)
            std::this_thread::sleep_for(10ms);
        } else {
            // Nothing arrived.
        }
    }
}
#endif

void FakeElm327::receive(const char* const data, const std::size_t size) {
    for (std::size_t index{0U}; index < size; ++index) {
        const char character{data[index]};
        if (character == '\r') {
            const std::string received{input_};
            input_.clear();
            process(received);
        } else if ((character != ' ') && (static_cast<unsigned char>(character) >= 0x20U)) {
            // The ELM327 ignores spaces and control characters, and takes either case.
            input_.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
        } else {
            // Ignored.
        }
    }
}

void FakeElm327::process(const std::string& received) {
    // A lone "\r" repeats the last command.
    const std::string command{received.empty() ? last_command_ : received};
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        commands_.push_back(command);
    }
    if ((!behaviour_.silent) && (!command.empty())) {
        // The adapter echoes the characters as they arrive, before the command changes any setting.
        std::string output{echo_ ? (received + (linefeeds_ ? "\r\n" : "\r")) : std::string{}};
        const bool at{(command.size() >= 2U) && (command.compare(0U, 2U, "AT") == 0)};
        const std::vector<std::string> lines{at ? at_command(command) : obd_request(command)};
        last_command_ = command;
        const std::string end_of_line{linefeeds_ ? "\r\n" : "\r"};
        for (const std::string& line : lines) {
            output += line + end_of_line;
        }
        output += end_of_line + ">";
        send(output);
    }
}

std::vector<std::string> FakeElm327::at_command(const std::string& command) {
    const std::string name{command.substr(2U)};
    std::vector<std::string> lines{"OK"};
    if ((name == "Z") || (name == "WS")) {
        std::this_thread::sleep_for(behaviour_.reset_delay);
        reset();
        lines = {"", behaviour_.identity};
    } else if (name == "I") {
        lines = {behaviour_.identity};
    } else if (name == "@1") {
        lines = {behaviour_.description};
    } else if (name == "RV") {
        const std::lock_guard<std::mutex> lock{mutex_};
        lines = {vehicle_.voltage};
    } else if (name == "DP") {
        lines = {describe_protocol()};
    } else if (name == "DPN") {
        lines = {protocol_number()};
    } else if ((name == "E0") || (name == "E1")) {
        echo_ = name == "E1";
    } else if ((name == "L0") || (name == "L1")) {
        linefeeds_ = name == "L1";
    } else if ((name == "S0") || (name == "S1")) {
        spaces_ = name == "S1";
    } else if ((name.size() >= 3U) && ((name.compare(0U, 2U, "SP") == 0) || (name.compare(0U, 2U, "TP") == 0))) {
        // ATSP6 selects protocol 6; ATSP0 and ATSPA6 (automatic, trying 6 first) select automatic.
        const std::string argument{name.substr(2U)};
        const bool automatic{(argument.size() == 2U) && (argument[0U] == 'A')};
        const char protocol{argument.back()};
        if (((argument.size() == 1U) || automatic) && (kProtocols.find(protocol) != std::string_view::npos)) {
            protocol_ = automatic ? '0' : protocol;
            connected_ = false;
        } else {
            lines = {"?"};
        }
    } else if (name == "PC") {
        connected_ = false;  // Protocol close.
    } else if (!is_accepted_setting(name)) {
        lines = {"?"};
    } else if (name.compare(0U, 2U, "SH") == 0) {
        header_ = name.substr(2U);  // Decides which module answers UDS requests.
    } else {
        // A setting without effect here.
    }
    return lines;
}

std::vector<std::string> FakeElm327::obd_request(const std::string& command) {
    std::string hex{command};
    // A last odd digit is the number of replies to wait for (ELM327 v1.3); it changes nothing here.
    if ((hex.size() >= 3U) && ((hex.size() % 2U) == 1U)) {
        hex.pop_back();
    }
    std::vector<std::uint8_t> request{};
    std::vector<std::string> lines{};
    if ((!decode_hex(hex, request)) || request.empty()) {
        lines = {"?"};
    } else {
        if ((!connected_) && (protocol_ == '0')) {
            lines.push_back("SEARCHING...");
            std::this_thread::sleep_for(behaviour_.search_delay);
        }
        const std::lock_guard<std::mutex> lock{mutex_};
        if (!vehicle_.ignition_on) {
            lines.push_back("UNABLE TO CONNECT");
        } else if ((protocol_ != '0') && (protocol_ != vehicle_protocol())) {
            lines.push_back((kCanProtocols.find(protocol_) != std::string_view::npos) ? "CAN ERROR"
                                                                                       : "BUS INIT: ...ERROR");
        } else {
            connected_ = true;
            const std::vector<std::vector<std::uint8_t>> messages{vehicle_messages(request)};
            if (messages.empty()) {
                lines.push_back("NO DATA");
            }
            for (const std::vector<std::uint8_t>& message : messages) {
                add_lines(message, lines);
            }
        }
    }
    return lines;
}

// The functions below are called with mutex_ held.

std::vector<std::vector<std::uint8_t>> FakeElm327::vehicle_messages(const std::vector<std::uint8_t>& request) {
    std::vector<std::vector<std::uint8_t>> messages{};
    const std::uint8_t service{request[0U]};
    const std::uint8_t reply{static_cast<std::uint8_t>(service + 0x40U)};
    if ((service == 0x01U) && (request.size() == 2U)) {
        const std::uint8_t pid{request[1U]};
        if ((pid % 0x20U) == 0U) {
            // PID 00 always answers, and 20, 40, ... when the range before announces them.
            if ((pid == 0U) || has_pid_above(pid)) {
                std::vector<std::uint8_t> message{reply, pid};
                const std::vector<std::uint8_t> bitmap{support_bitmap(pid)};
                message.insert(message.end(), bitmap.begin(), bitmap.end());
                messages.push_back(message);
            }
        } else {
            const auto entry = vehicle_.pids.find(pid);
            if (entry != vehicle_.pids.end()) {
                std::vector<std::uint8_t> message{reply, pid};
                message.insert(message.end(), entry->second.begin(), entry->second.end());
                messages.push_back(message);
            }
        }
    } else if ((request.size() == 1U) && (service == 0x03U)) {
        messages = dtc_messages(service, vehicle_.stored_dtcs);
    } else if ((request.size() == 1U) && (service == 0x07U)) {
        messages = dtc_messages(service, vehicle_.pending_dtcs);
    } else if ((request.size() == 1U) && (service == 0x0AU)) {
        messages = dtc_messages(service, vehicle_.permanent_dtcs);
    } else if ((request.size() == 1U) && (service == 0x04U)) {
        // Clearing removes the stored and pending codes; the permanent ones stay until the ECU
        // finds the fault gone.
        vehicle_.stored_dtcs.clear();
        vehicle_.pending_dtcs.clear();
        messages.push_back({reply});
    } else if ((service == 0x09U) && (request.size() == 2U) && (request[1U] == 0x02U)) {
        messages = vin_messages();
    } else if ((service == 0x10U) || (service == 0x11U) || (service == 0x22U) || (service == 0x27U) ||
               (service == 0x2EU) || (service == 0x31U) || (service == 0x3EU)) {
        messages = uds_messages(request);
    } else {
        // Not simulated: "NO DATA".
    }
    return messages;
}

std::vector<std::vector<std::uint8_t>> FakeElm327::uds_messages(const std::vector<std::uint8_t>& request) {
    std::vector<std::vector<std::uint8_t>> messages{};
    std::map<std::uint16_t, std::vector<std::uint8_t>>* const dids{addressed_module()};
    const std::uint8_t service{request[0U]};
    const std::uint8_t positive{static_cast<std::uint8_t>(service + 0x40U)};
    if (dids == nullptr) {
        // No module listens to this header: "NO DATA", as when it is not fitted.
    } else if (service == 0x3EU) {
        // TesterPresent: a positive reply, unless the suppress-positive-response bit asks for none
        // (the adapter then reports "NO DATA", as with a real ECU).
        const std::uint8_t sub{(request.size() >= 2U) ? request[1U] : std::uint8_t{0U}};
        if ((sub & 0x80U) == 0U) {
            messages.push_back({positive, sub});
        }
    } else if ((service == 0x10U) && (request.size() == 2U)) {
        // DiagnosticSessionControl: echo the session and some timing parameters.
        session_ = request[1U];
        messages.push_back({positive, request[1U], 0x00U, 0x32U, 0x01U, 0xF4U});
    } else if ((service == 0x11U) && (request.size() == 2U)) {
        session_ = 0x01U;  // ECUReset: the module starts again in the default session.
        messages.push_back({positive, request[1U]});
    } else if ((service == 0x22U) && (request.size() == 3U)) {
        const std::uint16_t did{
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(request[1U]) << 8U) | request[2U])};
        const auto entry = dids->find(did);
        if (entry != dids->end()) {
            std::vector<std::uint8_t> message{positive, request[1U], request[2U]};
            message.insert(message.end(), entry->second.begin(), entry->second.end());
            messages.push_back(message);
        } else {
            messages.push_back({0x7FU, service, 0x31U});  // requestOutOfRange.
        }
    } else if ((service == 0x2EU) && (request.size() >= 3U)) {
        // WriteDataByIdentifier: store the new value so a later read shows it changed.
        const std::uint16_t did{
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(request[1U]) << 8U) | request[2U])};
        if (vehicle_.locked_dids.count(did) > 0U) {
            messages.push_back({0x7FU, service, 0x33U});  // securityAccessDenied.
        } else if ((vehicle_.session_protected_dids.count(did) > 0U) && (session_ == 0x01U)) {
            messages.push_back({0x7FU, service, 0x7FU});  // serviceNotSupportedInActiveSession.
        } else {
            (*dids)[did] = std::vector<std::uint8_t>{request.begin() + 3, request.end()};
            messages.push_back({positive, request[1U], request[2U]});
        }
    } else if ((service == 0x31U) && (request.size() >= 4U)) {
        messages.push_back({positive, request[1U], request[2U], request[3U]});  // RoutineControl.
    } else if ((service == 0x27U) && (request.size() >= 2U)) {
        const std::uint8_t level{request[1U]};
        if ((level % 2U) == 1U) {
            messages.push_back({positive, level, 0x12U, 0x34U});  // requestSeed: a fixed seed.
        } else {
            messages.push_back({positive, level});  // sendKey: the simulator accepts any key.
        }
    } else {
        messages.push_back({0x7FU, service, 0x13U});  // incorrectMessageLengthOrInvalidFormat.
    }
    return messages;
}

std::map<std::uint16_t, std::vector<std::uint8_t>>* FakeElm327::addressed_module() {
    std::map<std::uint16_t, std::vector<std::uint8_t>>* dids{nullptr};
    if (header_.empty() || (header_ == "7DF") || (header_ == "7E0")) {
        dids = &vehicle_.data_by_identifier;  // The engine module, also through the functional address.
    } else {
        const auto module = vehicle_.modules.find(header_);
        if (module != vehicle_.modules.end()) {
            dids = &module->second;
        }
    }
    return dids;
}

std::vector<std::uint8_t> FakeElm327::support_bitmap(const std::uint8_t base) const {
    std::vector<std::uint8_t> bitmap(4U, std::uint8_t{0U});
    const std::uint32_t first{std::uint32_t{base} + 1U};
    const std::uint32_t last{std::uint32_t{base} + 0x20U};
    for (const auto& entry : vehicle_.pids) {
        const std::uint32_t pid{entry.first};
        if ((pid >= first) && (pid <= last)) {
            const std::uint32_t bit{pid - first};
            bitmap[bit / 8U] = static_cast<std::uint8_t>(bitmap[bit / 8U] | (0x80U >> (bit % 8U)));
        }
    }
    // The last bit announces the next range when the vehicle has PIDs beyond this one.
    if (has_pid_above(last)) {
        bitmap[3U] = static_cast<std::uint8_t>(bitmap[3U] | 0x01U);
    }
    return bitmap;
}

bool FakeElm327::has_pid_above(const std::uint32_t pid) const {
    bool found{false};
    for (const auto& entry : vehicle_.pids) {
        found = found || (std::uint32_t{entry.first} > pid);
    }
    return found;
}

std::vector<std::vector<std::uint8_t>> FakeElm327::dtc_messages(const std::uint8_t service,
                                                                const std::vector<std::uint16_t>& dtcs) const {
    const std::uint8_t reply{static_cast<std::uint8_t>(service + 0x40U)};
    std::vector<std::vector<std::uint8_t>> messages{};
    if (vehicle_.can) {
        // CAN: the number of DTCs, then the DTCs, in one message.
        std::vector<std::uint8_t> message{reply, static_cast<std::uint8_t>(dtcs.size())};
        for (const std::uint16_t dtc : dtcs) {
            message.push_back(static_cast<std::uint8_t>(dtc >> 8U));
            message.push_back(static_cast<std::uint8_t>(dtc & 0xFFU));
        }
        messages.push_back(message);
    } else {
        // ISO 9141-2: three DTCs per message, padded with 00 00; one message even without DTCs.
        std::size_t index{0U};
        do {
            std::vector<std::uint8_t> message{reply};
            for (std::size_t slot{0U}; slot < 3U; ++slot) {
                const std::uint16_t dtc{(index < dtcs.size()) ? dtcs[index] : std::uint16_t{0U}};
                message.push_back(static_cast<std::uint8_t>(dtc >> 8U));
                message.push_back(static_cast<std::uint8_t>(dtc & 0xFFU));
                ++index;
            }
            messages.push_back(message);
        } while (index < dtcs.size());
    }
    return messages;
}

std::vector<std::vector<std::uint8_t>> FakeElm327::vin_messages() const {
    std::vector<std::uint8_t> characters{};
    for (const char character : vehicle_.vin) {
        characters.push_back(static_cast<std::uint8_t>(character));
    }
    std::vector<std::vector<std::uint8_t>> messages{};
    if (vehicle_.can) {
        // CAN: 49 02, the number of data items (1), and the VIN, in one multi-frame message.
        std::vector<std::uint8_t> message{0x49U, 0x02U, 0x01U};
        message.insert(message.end(), characters.begin(), characters.end());
        messages.push_back(message);
    } else {
        // ISO 9141-2: five messages 49 02 <n> of four bytes; three bytes 00 pad the 17 characters.
        std::vector<std::uint8_t> data{0U, 0U, 0U};
        data.insert(data.end(), characters.begin(), characters.end());
        data.resize(20U, std::uint8_t{0U});
        for (std::size_t number{1U}; number <= 5U; ++number) {
            std::vector<std::uint8_t> message{0x49U, 0x02U, static_cast<std::uint8_t>(number)};
            for (std::size_t offset{0U}; offset < 4U; ++offset) {
                message.push_back(data[((number - 1U) * 4U) + offset]);
            }
            messages.push_back(message);
        }
    }
    return messages;
}

void FakeElm327::add_lines(const std::vector<std::uint8_t>& message, std::vector<std::string>& lines) const {
    if ((!vehicle_.can) || (message.size() <= kSingleFrameSize)) {
        lines.push_back(format_bytes(message));
    } else {
        // A multi-frame message: its length in three hexadecimal digits, then the numbered frames
        // of 6, 7, 7, ... bytes, the last one padded with 00.
        lines.push_back(hex_digit(message.size() >> 8U) + hex_byte(static_cast<std::uint8_t>(message.size() & 0xFFU)));
        std::size_t offset{0U};
        for (std::size_t frame{0U}; offset < message.size(); ++frame) {
            const std::size_t frame_size{(frame == 0U) ? kFirstFrameSize : kConsecutiveFrameSize};
            std::vector<std::uint8_t> part{};
            for (std::size_t index{0U}; index < frame_size; ++index) {
                part.push_back(((offset + index) < message.size()) ? message[offset + index] : std::uint8_t{0U});
            }
            lines.push_back(hex_digit(frame) + (spaces_ ? ": " : ":") + format_bytes(part));
            offset += frame_size;
        }
    }
}

std::string FakeElm327::format_bytes(const std::vector<std::uint8_t>& bytes) const {
    std::string text{};
    for (const std::uint8_t byte : bytes) {
        text += hex_byte(byte);
        if (spaces_) {
            text += ' ';  // A real ELM327 follows every byte with a space, the last one too.
        }
    }
    return text;
}

std::string FakeElm327::describe_protocol() const {
    const std::lock_guard<std::mutex> lock{mutex_};
    std::string text{};
    if (protocol_ == '0') {
        text = connected_ ? ("AUTO, " + protocol_name(vehicle_protocol())) : std::string{"AUTO"};
    } else {
        text = protocol_name(protocol_);
    }
    return text;
}

std::string FakeElm327::protocol_number() const {
    const std::lock_guard<std::mutex> lock{mutex_};
    std::string text{};
    if (protocol_ == '0') {
        text = std::string{"A"} + (connected_ ? vehicle_protocol() : '0');
    } else {
        text = std::string(1U, protocol_);
    }
    return text;
}

char FakeElm327::vehicle_protocol() const {
    return vehicle_.can ? '6' : '3';
}

void FakeElm327::reset() {
    echo_ = true;
    linefeeds_ = behaviour_.linefeeds;
    spaces_ = true;
    connected_ = false;
    header_.clear();  // The modules keep their session: a reset of the adapter does not reach them.
}

#if defined(_WIN32)
void FakeElm327::send(const std::string& text) const {
    // On the simulation thread, between two reads: the event is free.
    std::size_t written{0U};
    for (int attempt{0}; (written < text.size()) && (attempt < kSendAttempts); ++attempt) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event_;
        const DWORD size{static_cast<DWORD>(text.size() - written)};
        if ((::WriteFile(pipe_, text.data() + written, size, nullptr, &overlapped) != FALSE) ||
            (::GetLastError() == ERROR_IO_PENDING)) {
            // A program that does not read for a long time loses the rest of the reply.
            if (::WaitForSingleObject(event_, kSendMilliseconds) != WAIT_OBJECT_0) {
                static_cast<void>(::CancelIoEx(pipe_, &overlapped));
            }
            DWORD count{0U};
            if ((::GetOverlappedResult(pipe_, &overlapped, &count, TRUE) != FALSE) && (count > 0U)) {
                written += static_cast<std::size_t>(count);
            } else {
                attempt = kSendAttempts;
            }
        } else {
            attempt = kSendAttempts;  // Nobody has the pipe open: drop the output.
        }
    }
}
#else
void FakeElm327::send(const std::string& text) const {
    std::size_t written{0U};
    for (int attempt{0}; (written < text.size()) && (attempt < kSendAttempts); ++attempt) {
        // On a socket, MSG_NOSIGNAL: a closed other end is EPIPE rather than a SIGPIPE.
        const ssize_t count{socket_ ? ::send(master_, text.data() + written, text.size() - written, MSG_NOSIGNAL)
                                    : ::write(master_, text.data() + written, text.size() - written)};
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if ((count < 0) && (errno == EAGAIN)) {
            std::this_thread::sleep_for(1ms);  // The buffer is full: wait for the program to read.
        } else {
            attempt = kSendAttempts;  // Nobody has the device open: drop the output.
        }
    }
}
#endif

}  // namespace sim
}  // namespace elm327
