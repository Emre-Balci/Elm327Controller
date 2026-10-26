/// @file worker.hpp
/// @brief The thread that talks to the adapter for the GUI.
///
/// A user interface must never wait for the serial port: a reset takes a second, a protocol search
/// more than ten. The Worker therefore owns the Controller and runs it on a thread of its own. The
/// user interface posts Requests and reads back what the worker found, and neither side ever waits
/// for the other:
/// - the State, a snapshot of everything the worker knows (the connection, the adapter, the
///   supported PIDs, the trouble codes, the VIN, what it is busy with), through a TripleBuffer;
/// - Samples of PID values, through an SpscQueue, so that no value of a monitor is lost;
/// - TraceLines, the traffic with the adapter and the worker's messages, through another SpscQueue;
/// - UdsResults, the outcome of each UDS request of expert mode, through a third one, so that the
///   three steps of a write (read, write, read again) all arrive;
/// - FeatureResults, what each feature of the Features tab was found to be or turned into, through
///   a fourth one.
///
/// cancel() interrupts the operation in progress and drops the requests posted before it, like
/// Ctrl+C in the shell.
///
/// The Worker is GUI-independent: elm327gui draws its results with ImGui, and the tests drive it
/// directly. It embeds its queues and the Controller (about 300 KiB): give it static storage
/// duration or allocate it once.

#ifndef ELM327_GUI_WORKER_HPP
#define ELM327_GUI_WORKER_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/concurrency/spsc_queue.hpp"
#include "common/concurrency/triple_buffer.hpp"
#include "common/containers/static_string.hpp"
#include "common/containers/static_vector.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "common/system/thread.hpp"
#include "common/time/clock.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/controller/controller.hpp"
#include "elm327/expert/feature.hpp"
#include "elm327/expert/uds.hpp"
#include "elm327/obd/dtc.hpp"
#include "elm327/obd/message.hpp"
#include "elm327/obd/vin.hpp"

namespace elm327 {
namespace gui {

/// Most PIDs a monitor reads in each sample, and most readings in a Sample.
inline constexpr std::size_t kMaxMonitored{16U};

/// The interval of a monitor unless a Request says otherwise. A constant, so that the structures
/// below copy it: the converting constructor of a duration is not noexcept, a copy is.
inline constexpr common::Duration kDefaultInterval{std::chrono::seconds{1}};

enum class RequestKind : std::uint8_t {
    kConnect,       ///< Connect with Request::settings, then read the adapter and the supported PIDs.
    kDisconnect,    ///< Close the connection (and stop a monitor).
    kReadInfo,      ///< Read the adapter's description, voltage and the vehicle's protocol again.
    kReadPids,      ///< Read the supported PIDs again.
    kReadValues,    ///< Read the PIDs in Request::pids once.
    kStartMonitor,  ///< Read the PIDs in Request::pids every Request::interval, until stopped.
    kStopMonitor,
    kReadDtcs,      ///< Read the MIL status and the stored, pending and permanent DTCs.
    kReadVin,
    kRaw,           ///< Send Request::command to the adapter as it is.
    kUds,           ///< Send the UDS request Request::command and report a UdsResult (expert mode).
    kUdsWrite,      ///< Write Request::value to Request::did with a backup: read, write, read again.
    kFeatureRead,     ///< Read the state of Request::feature and report a FeatureResult.
    kFeatureChange,   ///< Turn Request::feature on or off (Request::feature_on).
    kFeatureRestore,  ///< Write Request::backup back to the setting of Request::feature.
    kStop,          ///< End the thread.
};

/// A short name of what a UDS request is for, shown with its result, e.g. "read" or "current value".
using UdsLabel = common::StaticString<24U>;

struct Request final {
    RequestKind kind{RequestKind::kStop};
    ConnectionSettings settings{};
    /// kConnect: connect in read-only mode (refuse anything that could change the vehicle).
    bool read_only{false};
    common::ByteSet pids{};
    common::Duration interval{kDefaultInterval};
    /// kRaw: the command; kUds: the request in hexadecimal, e.g. "22F190".
    common::StaticString<Adapter::kMaxCommandLength> command{};
    /// kUds: what the request is for.
    UdsLabel label{};
    /// kUdsWrite: the data identifier and the value to write.
    std::uint16_t did{0U};
    common::StaticVector<std::uint8_t, expert::kMaxRequestData> value{};
    /// kFeatureRead, kFeatureChange, kFeatureRestore: the feature, its position in the window's
    /// list (returned in the FeatureResult), whether to turn it on, and the backup to restore.
    expert::Feature feature{};
    std::uint16_t feature_index{0U};
    bool feature_on{false};
    common::StaticVector<std::uint8_t, expert::kMaxFeatureValue> backup{};
    std::uint32_t generation{0U};  ///< Set by Worker::post(); see Worker::cancel().
};

/// What the worker is doing.
enum class Activity : std::uint8_t {
    kIdle,
    kConnecting,
    kReadingInfo,
    kReadingPids,
    kReadingValues,
    kMonitoring,
    kReadingDtcs,
    kReadingVin,
    kSendingCommand,
    kTalkingToEcu,      ///< A UDS request of expert mode.
    kCheckingFeatures,  ///< Reading the state of a feature.
    kChangingFeature,   ///< Turning a feature on or off, or restoring it.
};

/// @return a description of @p activity for the user, e.g. "Reading trouble codes".
[[nodiscard]] const char* to_string(Activity activity) noexcept;

/// The DTCs of one kind, or why they could not be read.
struct DtcSection final {
    DtcKind kind{DtcKind::kStored};
    /// kOk with the codes; kNotFound when no ECU answered, kNotSupported when they reject the
    /// service, or another error.
    common::Status status{common::Status::kNotFound};
    DtcList codes{};
};

/// A message of the worker, e.g. "Connected to ELM327 v1.5" or why a request failed.
using MessageText = common::StaticString<192U>;

/// Everything the worker knows, as one value.
struct State final {
    /// Increases with every publication.
    std::uint64_t version{0U};
    Activity activity{Activity::kIdle};
    bool connected{false};
    /// Read-only mode is in effect: writes to the vehicle are refused (see Controller).
    bool read_only{false};
    /// The settings of the connection, or of the last attempt.
    ConnectionSettings settings{};
    AdapterText identity{};
    AdapterText description{};
    AdapterText voltage{};
    /// The vehicle's protocol, e.g. "AUTO, ISO 15765-4 (CAN 11/500)"; empty while unknown.
    AdapterText protocol{};
    bool pids_known{false};
    common::ByteSet supported_pids{};
    bool monitoring{false};
    common::ByteSet monitored_pids{};
    common::Duration interval{kDefaultInterval};
    bool dtcs_read{false};
    bool mil_known{false};
    MilStatus mil{};
    std::array<DtcSection, 3U> dtcs{};
    bool vin_read{false};
    common::Status vin_status{common::Status::kNotFound};
    Vin vin{};
    /// The last message, and whether it reports a failure; message_count counts them.
    MessageText message{};
    bool message_failed{false};
    std::uint32_t message_count{0U};
};

/// Values of PIDs read at one time.
struct Sample final {
    /// Time since the monitor started; zero for a single read.
    common::Duration time{};
    bool monitored{false};
    common::StaticVector<PidReading, kMaxMonitored> readings{};
};

/// A line of the traffic with the adapter, or a message of the worker.
struct TraceLine final {
    enum class Kind : std::uint8_t {
        kCommand,  ///< Sent to the adapter.
        kReply,    ///< Received from the adapter.
        kNote,     ///< A message of the worker.
        kError,    ///< A message about a failure.
    };

    Kind kind{Kind::kNote};
    MessageText text{};
};

/// The outcome of one UDS request of expert mode.
struct UdsResult final {
    enum class Outcome : std::uint8_t {
        kPositive,  ///< The ECU accepted the request; exchange holds its reply.
        kNegative,  ///< The ECU rejected it; exchange.nrc says why.
        kRefused,   ///< Read-only mode refused it before anything was sent.
        kFailed,    ///< No usable reply, e.g. "NO DATA", a timeout, or a reply that is not UDS.
    };

    Outcome outcome{Outcome::kFailed};
    UdsLabel label{};
    /// The request as sent, in hexadecimal, e.g. "22F190".
    common::StaticString<Adapter::kMaxCommandLength> request{};
    expert::UdsExchange exchange{};
    /// Why the outcome is not kPositive, in words, e.g. "securityAccessDenied (0x33)" or "NO DATA".
    MessageText reason{};
};

/// The outcome of a feature request: what the feature is now, and what was written.
struct FeatureResult final {
    RequestKind kind{RequestKind::kFeatureRead};  ///< kFeatureRead, kFeatureChange or kFeatureRestore.
    std::uint16_t index{0U};                      ///< Request::feature_index.
    expert::FeatureReading reading{};
};

/// Called on the worker's thread after it published something, e.g. to wake up the user
/// interface (glfwPostEmptyEvent()). Must not block.
using NotifyFunction = void (*)(void* context) noexcept;

class Worker final {
public:
    static constexpr std::size_t kRequestCapacity{16U};
    static constexpr std::size_t kSampleCapacity{64U};
    static constexpr std::size_t kTraceCapacity{512U};
    static constexpr std::size_t kUdsCapacity{64U};
    static constexpr std::size_t kFeatureCapacity{expert::kMaxFeatures};

    Worker() noexcept = default;

    /// Stops the thread if it runs.
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    /// Starts the thread. @p notify (nullptr for none) is called after each publication.
    /// @return kOk; kAlreadyRunning; kResourceLimit or kSystemError if the descriptors or the thread
    ///         could not be created.
    common::Status start(NotifyFunction notify, void* context) noexcept;

    /// Cancels what runs, ends the thread (which closes the connection) and waits for it.
    void stop() noexcept;

#if !defined(_WIN32)
    /// See Controller::set_link_opener(), e.g. the Android app's Bluetooth links. Call while the
    /// thread does not run: the opener then runs on it, and cancel() ends its wait.
    void set_link_opener(const LinkOpener opener, void* const context) noexcept {
        controller_.set_link_opener(opener, context);
    }
#endif

    // The functions below are for one user-interface thread.

    /// Posts @p request. @return false if the queue is full or the worker does not run.
    [[nodiscard]] bool post(const Request& request) noexcept;

    /// Posts a request of @p kind without parameters, e.g. RequestKind::kReadDtcs.
    [[nodiscard]] bool post(RequestKind kind) noexcept;

    /// Interrupts the operation in progress, stops a monitor, and drops the requests posted
    /// before. The requests posted afterwards run.
    void cancel() noexcept;

    /// Takes the newest State, if one was published since the last call. @return true if state()
    /// changed.
    [[nodiscard]] bool update_state() noexcept {
        return state_buffer_.update();
    }

    /// The State taken by the last update_state(). Valid until the next update_state().
    [[nodiscard]] const State& state() const noexcept {
        return state_buffer_.read();
    }

    /// @return true with the oldest Sample not taken yet in @p sample.
    [[nodiscard]] bool take_sample(Sample& sample) noexcept {
        return samples_.try_pop(sample);
    }

    /// @return true with the oldest TraceLine not taken yet in @p line.
    [[nodiscard]] bool take_trace(TraceLine& line) noexcept {
        return trace_.try_pop(line);
    }

    /// @return true with the oldest UdsResult not taken yet in @p result.
    [[nodiscard]] bool take_uds_result(UdsResult& result) noexcept {
        return uds_results_.try_pop(result);
    }

    /// @return true with the oldest FeatureResult not taken yet in @p result.
    [[nodiscard]] bool take_feature_result(FeatureResult& result) noexcept {
        return feature_results_.try_pop(result);
    }

    /// Samples and trace lines dropped because the user interface did not take them in time.
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    static void entry(void* context) noexcept;
    void close_descriptors() noexcept;
    static void trace_traffic(TraceEvent event, common::Span<const char> text, void* context) noexcept;

    // The functions below run on the worker's thread.
    void run() noexcept;
    void handle(const Request& request) noexcept;
    void connect(const ConnectionSettings& settings, bool read_only) noexcept;
    void disconnect() noexcept;
    /// @return true if the vehicle answered.
    bool read_info() noexcept;
    void read_pids() noexcept;
    void read_values(const common::ByteSet& pids) noexcept;
    void start_monitor(const Request& request) noexcept;
    void sample() noexcept;
    void read_dtcs() noexcept;
    void read_vin() noexcept;
    void send(const Request& request) noexcept;
    void uds(const Request& request) noexcept;
    void uds_write(const Request& request) noexcept;
    /// Sends the UDS request @p command, decodes the reply, and publishes a UdsResult labelled
    /// @p label, with a message for the status line. @return its outcome.
    UdsResult::Outcome exchange_uds(std::string_view label, std::string_view command) noexcept;
    /// Reads, changes or restores a feature and publishes a FeatureResult.
    void run_feature(const Request& request) noexcept;

    /// Queries @p command into @p text; "unknown" if the adapter does not know the command.
    common::Status query(std::string_view command, AdapterText& text) noexcept;
    /// @return true if connected; otherwise tells the user to connect first.
    [[nodiscard]] bool require_connection() noexcept;
    /// Forgets everything learned from the adapter and the vehicle.
    void forget() noexcept;
    void push_sample(const Sample& sample) noexcept;

    /// Starts an operation: drains the cancel descriptor, and publishes @p activity.
    void begin(Activity activity) noexcept;
    /// Ends an operation: notices a lost connection, and publishes the State.
    void finish() noexcept;
    /// Reports that @p what failed with @p status, explained with the Controller's last reply.
    void fail(const char* what, common::Status status) noexcept;
    void note(common::Span<const char> text, bool failed) noexcept;
    void push_trace(TraceLine::Kind kind, common::Span<const char> text) noexcept;
    void publish() noexcept;
    /// @return true if a request of an earlier generation than the last cancel() is to be dropped.
    [[nodiscard]] bool cancelled(std::uint32_t generation) const noexcept;

    // Shared between the threads.
    common::SpscQueue<Request, kRequestCapacity> requests_{};
    common::TripleBuffer<State> state_buffer_{};
    common::SpscQueue<Sample, kSampleCapacity> samples_{};
    common::SpscQueue<TraceLine, kTraceCapacity> trace_{};
    common::SpscQueue<UdsResult, kUdsCapacity> uds_results_{};
    common::SpscQueue<FeatureResult, kFeatureCapacity> feature_results_{};
    std::atomic<std::uint32_t> generation_{0U};
    std::atomic<std::uint64_t> dropped_{0U};
    /// An eventfd (on Windows, a manual-reset event): a request was posted.
    common::NativeHandle wake_descriptor_{common::kInvalidNativeHandle};
    /// Another one: the Controller's cancel descriptor.
    common::NativeHandle cancel_descriptor_{common::kInvalidNativeHandle};
    NotifyFunction notify_{nullptr};
    void* notify_context_{nullptr};
    common::RtThread thread_{};
    bool running_{false};  ///< User-interface thread only.

    // The worker's thread only.
    Controller controller_{};
    expert::FeatureCoder features_{controller_};  ///< After controller_, which it uses.
    Response response_{};         ///< Replies to raw commands and UDS requests.
    MessageList uds_messages_{};  ///< Scratch for decoding UDS replies.
    State state_{};
    bool stopping_{false};
    std::uint32_t monitor_generation_{0U};
    common::TimePoint monitor_start_{};
    common::TimePoint next_sample_{};
};

}  // namespace gui
}  // namespace elm327

#endif  // ELM327_GUI_WORKER_HPP
