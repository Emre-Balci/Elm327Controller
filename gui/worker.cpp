#include "gui/worker.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "common/protocol/text.hpp"
#include "common/time/deadline.hpp"
#include "elm327/controller/explain.hpp"
#include "elm327/controller/timing.hpp"

namespace elm327 {
namespace gui {
namespace {

constexpr std::uint8_t kMilStatusPid{0x01U};

/// The longest wait of an idle worker. A request wakes it up at once; this only bounds the wait.
constexpr common::Duration kIdleWait{std::chrono::milliseconds{500}};

/// Rounds of the worker's loop, one per wake-up: a bound, not a limit anybody reaches.
constexpr std::uint64_t kMaxRounds{std::numeric_limits<std::uint64_t>::max()};

/// Attempts of stop() to post kStop to a full queue, 1 ms apart.
constexpr std::uint32_t kStopAttempts{2000U};

constexpr std::array<DtcKind, 3U> kDtcKinds{{DtcKind::kStored, DtcKind::kPending, DtcKind::kPermanent}};

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

/// Makes the eventfd @p descriptor unreadable again.
void drain(const std::int32_t descriptor) noexcept {
    std::uint64_t value{0U};
    const ssize_t count{::read(descriptor, &value, sizeof(value))};
    static_cast<void>(count);  // Nothing to drain is fine.
}

/// Makes the eventfd @p descriptor readable.
void notify_event(const std::int32_t descriptor) noexcept {
    const std::uint64_t one{1U};
    const ssize_t count{::write(descriptor, &one, sizeof(one))};
    static_cast<void>(count);  // Fails only when the counter is about to overflow: it is readable then.
}

/// Appends as much of @p part to @p text as fits: a message cut short beats no message.
void put(MessageText& text, const common::Span<const char> part) noexcept {
    const std::size_t room{MessageText::capacity() - text.size()};
    static_cast<void>(text.append((part.size() <= room) ? part : part.first(room)));
}

void put(MessageText& text, const std::string_view part) noexcept {
    put(text, span_of(part));
}

/// The number of PIDs in @p pids that carry a value, without the support PIDs 00, 20, ...
std::uint32_t count_pids(const common::ByteSet& pids) noexcept {
    std::uint32_t count{0U};
    for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
        const std::uint8_t pid{static_cast<std::uint8_t>(value)};
        count += (pids.contains(pid) && (!is_support_pid(pid))) ? 1U : 0U;
    }
    return count;
}

}  // namespace

const char* to_string(const Activity activity) noexcept {
    const char* text{"Idle"};
    switch (activity) {
        case Activity::kIdle:
            text = "Idle";
            break;
        case Activity::kConnecting:
            text = "Connecting";
            break;
        case Activity::kReadingInfo:
            text = "Reading the adapter";
            break;
        case Activity::kReadingPids:
            text = "Reading the supported PIDs";
            break;
        case Activity::kReadingValues:
            text = "Reading values";
            break;
        case Activity::kMonitoring:
            text = "Monitoring";
            break;
        case Activity::kReadingDtcs:
            text = "Reading trouble codes";
            break;
        case Activity::kReadingVin:
            text = "Reading the VIN";
            break;
        case Activity::kSendingCommand:
            text = "Sending a command";
            break;
    }
    return text;
}

Worker::~Worker() {
    stop();
}

common::Status Worker::start(const NotifyFunction notify, void* const context) noexcept {
    common::Status status{running_ ? common::Status::kAlreadyRunning : common::Status::kOk};
    if (is_ok(status)) {
        const int flags{
            static_cast<int>(static_cast<std::uint32_t>(EFD_CLOEXEC) | static_cast<std::uint32_t>(EFD_NONBLOCK))};
        wake_descriptor_ = ::eventfd(0U, flags);
        cancel_descriptor_ = ::eventfd(0U, flags);
        status = ((wake_descriptor_ >= 0) && (cancel_descriptor_ >= 0)) ? common::Status::kOk
                                                                         : common::Status::kResourceLimit;
    }
    if (is_ok(status)) {
        notify_ = notify;
        notify_context_ = context;
        controller_.set_cancel_descriptor(cancel_descriptor_);
        controller_.set_trace(&trace_traffic, this);
        stopping_ = false;
        publish();  // The first State, before the thread that writes the others starts.
        common::ThreadConfig config{};
        config.name = "elm327-worker";
        config.policy = common::SchedPolicy::kOther;
        config.priority = 0;
        status = thread_.start(config, &entry, this);
    }
    if (status != common::Status::kAlreadyRunning) {
        running_ = is_ok(status);
        if (!running_) {
            close_descriptors();
        }
    }
    return status;
}

void Worker::stop() noexcept {
    if (running_) {
        cancel();
        Request request{};
        request.kind = RequestKind::kStop;
        // The cancel makes the worker drop the requests that fill the queue.
        bool posted{post(request)};
        for (std::uint32_t attempt{0U}; (!posted) && (attempt < kStopAttempts); ++attempt) {
            static_cast<void>(common::sleep_for(std::chrono::milliseconds{1}));
            posted = post(request);
        }
        static_cast<void>(thread_.join());
        running_ = false;
        close_descriptors();
    }
}

void Worker::close_descriptors() noexcept {
    if (wake_descriptor_ >= 0) {
        static_cast<void>(::close(wake_descriptor_));
        wake_descriptor_ = -1;
    }
    if (cancel_descriptor_ >= 0) {
        static_cast<void>(::close(cancel_descriptor_));
        cancel_descriptor_ = -1;
    }
}

bool Worker::post(const Request& request) noexcept {
    bool posted{false};
    if (running_) {
        Request copy{request};
        copy.generation = generation_.load(std::memory_order_acquire);
        posted = requests_.try_push(copy);
        if (posted) {
            notify_event(wake_descriptor_);
        }
    }
    return posted;
}

bool Worker::post(const RequestKind kind) noexcept {
    Request request{};
    request.kind = kind;
    return post(request);
}

void Worker::cancel() noexcept {
    if (running_) {
        static_cast<void>(generation_.fetch_add(1U, std::memory_order_acq_rel));
        notify_event(cancel_descriptor_);  // Interrupts the operation in progress.
        notify_event(wake_descriptor_);    // Ends the wait of a monitor.
    }
}

void Worker::entry(void* const context) noexcept {
    static_cast<Worker*>(context)->run();
}

void Worker::trace_traffic(const TraceEvent event, const common::Span<const char> text, void* const context) noexcept {
    Worker* const worker{static_cast<Worker*>(context)};
    worker->push_trace((event == TraceEvent::kCommand) ? TraceLine::Kind::kCommand : TraceLine::Kind::kReply, text);
}

void Worker::run() noexcept {
    for (std::uint64_t round{0U}; (!stopping_) && (round < kMaxRounds); ++round) {
        Request request{};
        for (std::size_t taken{0U}; (!stopping_) && (taken < kRequestCapacity) && requests_.try_pop(request);
             ++taken) {
            if ((request.kind == RequestKind::kStop) || (!cancelled(request.generation))) {
                handle(request);
            }
        }
        if (state_.monitoring && cancelled(monitor_generation_)) {
            state_.monitoring = false;  // cancel() stops a monitor too.
            note(span_of("Monitoring stopped."), false);
            finish();
        }
        if (!stopping_) {
            if (state_.monitoring && (common::MonotonicClock::now() >= next_sample_)) {
                sample();
            }
            const common::Deadline deadline{state_.monitoring ? common::Deadline::at(next_sample_)
                                                              : common::Deadline::after(kIdleWait)};
            if ((!stopping_) && requests_.empty_approx()) {
                // A request posted since the check makes the descriptor readable: no wake-up is lost.
                static_cast<void>(wait_until(wake_descriptor_, deadline));
            }
            drain(wake_descriptor_);
        }
    }
    controller_.disconnect();
}

void Worker::handle(const Request& request) noexcept {
    switch (request.kind) {
        case RequestKind::kConnect:
            connect(request.settings);
            break;
        case RequestKind::kDisconnect:
            disconnect();
            break;
        case RequestKind::kReadInfo:
            static_cast<void>(read_info());
            break;
        case RequestKind::kReadPids:
            read_pids();
            break;
        case RequestKind::kReadValues:
            read_values(request.pids);
            break;
        case RequestKind::kStartMonitor:
            start_monitor(request);
            break;
        case RequestKind::kStopMonitor:
            if (state_.monitoring) {
                state_.monitoring = false;
                note(span_of("Monitoring stopped."), false);
                finish();
            }
            break;
        case RequestKind::kReadDtcs:
            read_dtcs();
            break;
        case RequestKind::kReadVin:
            read_vin();
            break;
        case RequestKind::kRaw:
            send(request);
            break;
        case RequestKind::kStop:
            stopping_ = true;
            break;
    }
}

void Worker::connect(const ConnectionSettings& settings) noexcept {
    begin(Activity::kConnecting);
    forget();
    state_.settings = settings;
    const common::Status status{controller_.connect(settings)};
    MessageText text{};
    if (is_ok(status)) {
        state_.connected = true;
        static_cast<void>(state_.identity.append(controller_.identity()));  // Cannot fail: a line fits.
        put(text, "Connected to ");
        put(text, controller_.identity());
        put(text, " on ");
        put(text, settings.device.text());
        put(text, ".");
        note(text.text(), false);
        finish();
        if (read_info()) {
            read_pids();
        }
    } else {
        const Explanation explanation{explain_connect_failure(controller_.connect_failure(), status)};
        put(text, "Cannot connect to ");
        put(text, settings.device.text());
        put(text, ": ");
        put(text, explanation.reason);
        if (!explanation.advice.empty()) {
            put(text, " (");
            put(text, explanation.advice);
            put(text, ")");
        }
        note(text.text(), true);
        finish();
    }
}

void Worker::disconnect() noexcept {
    if (controller_.connected()) {
        controller_.disconnect();
        forget();
        note(span_of("Disconnected."), false);
    }
    finish();
}

bool Worker::read_info() noexcept {
    bool vehicle{false};
    if (require_connection()) {
        begin(Activity::kReadingInfo);
        state_.protocol.clear();
        common::Status status{query("AT@1", state_.description)};
        if (is_ok(status)) {
            status = query("ATRV", state_.voltage);
        }
        if (!is_ok(status)) {
            fail("Querying the adapter", status);
        } else {
            // The first OBD request makes an adapter in automatic mode find the vehicle's protocol.
            status = controller_.connect_vehicle();
            if (!is_ok(status)) {
                fail("Connecting to the vehicle", status);
            } else {
                vehicle = true;
                status = query("ATDP", state_.protocol);
                if (!is_ok(status)) {
                    fail("Querying the adapter", status);
                }
            }
        }
        finish();
    }
    return vehicle;
}

void Worker::read_pids() noexcept {
    if (require_connection()) {
        begin(Activity::kReadingPids);
        const common::Status status{controller_.read_supported_pids()};
        if (is_ok(status)) {
            state_.pids_known = true;
            state_.supported_pids = controller_.supported_pids();
            MessageText text{};
            static_cast<void>(common::append_unsigned(text, count_pids(state_.supported_pids)));
            put(text, " PIDs supported.");
            note(text.text(), false);
        } else {
            fail("Reading the supported PIDs", status);
        }
        finish();
    }
}

void Worker::read_values(const common::ByteSet& pids) noexcept {
    if (require_connection()) {
        begin(Activity::kReadingValues);
        common::Status status{controller_.ensure_supported_pids()};
        Sample batch{};
        for (std::uint32_t value{1U}; is_ok(status) && (value <= 0xFFU); ++value) {
            const std::uint8_t pid{static_cast<std::uint8_t>(value)};
            if (pids.contains(pid) && controller_.supported_pids().contains(pid)) {
                const PidReading reading{controller_.read_pid(pid)};
                if ((reading.status == common::Status::kInterrupted) || (!controller_.connected())) {
                    status = reading.status;
                } else {
                    static_cast<void>(batch.readings.push_back(reading));  // Cannot fail: never full here.
                }
            }
            if (batch.readings.full()) {
                push_sample(batch);
                batch.readings.clear();
            }
        }
        if (!batch.readings.empty()) {
            push_sample(batch);
        }
        if (!is_ok(status)) {
            fail("Reading values", status);
        }
        finish();
    }
}

void Worker::start_monitor(const Request& request) noexcept {
    if (require_connection()) {
        // The PIDs are read in ascending order, at most kMaxMonitored of them.
        common::ByteSet pids{};
        std::size_t count{0U};
        for (std::uint32_t value{1U}; (value <= 0xFFU) && (count < kMaxMonitored); ++value) {
            const std::uint8_t pid{static_cast<std::uint8_t>(value)};
            if (request.pids.contains(pid)) {
                pids = pids.with(pid);
                ++count;
            }
        }
        state_.monitoring = count > 0U;
        state_.monitored_pids = pids;
        state_.interval = (request.interval > common::Duration::zero()) ? request.interval : kDefaultInterval;
        monitor_generation_ = request.generation;
        monitor_start_ = common::MonotonicClock::now();
        next_sample_ = monitor_start_;
        note(span_of(state_.monitoring ? "Monitoring started." : "No PIDs to monitor."), !state_.monitoring);
        finish();
    }
}

void Worker::sample() noexcept {
    const common::Status known{controller_.ensure_supported_pids()};
    Sample values{};
    values.monitored = true;
    values.time = common::MonotonicClock::now() - monitor_start_;
    bool complete{is_ok(known)};
    for (std::uint32_t value{1U}; complete && (value <= 0xFFU) && (!values.readings.full()); ++value) {
        const std::uint8_t pid{static_cast<std::uint8_t>(value)};
        if (state_.monitored_pids.contains(pid)) {
            const PidReading reading{controller_.read_pid(pid)};
            complete = (reading.status != common::Status::kInterrupted) && controller_.connected() &&
                       (!cancelled(monitor_generation_));
            if (complete) {
                static_cast<void>(values.readings.push_back(reading));  // Cannot fail: not full.
            }
        }
    }
    if (complete) {
        push_sample(values);
    } else if (!is_ok(known)) {
        state_.monitoring = false;
        fail("Monitoring", known);
    } else {
        // Interrupted, or the device is gone: finish() tells.
    }
    next_sample_ = next_activation(next_sample_, state_.interval, common::MonotonicClock::now());
    if (!controller_.connected()) {
        finish();
    }
}

void Worker::read_dtcs() noexcept {
    if (require_connection()) {
        begin(Activity::kReadingDtcs);
        common::Status status{controller_.connect_vehicle()};
        if (is_ok(status)) {
            state_.mil_known = false;
            if (controller_.supported_pids().contains(kMilStatusPid)) {
                const common::Result<MilStatus> mil{controller_.read_mil_status()};
                state_.mil_known = mil.has_value();
                state_.mil = mil.value_or(MilStatus{});
                if (mil.status() == common::Status::kInterrupted) {
                    status = mil.status();
                }
            }
            for (std::size_t index{0U}; is_ok(status) && controller_.connected() && (index < kDtcKinds.size());
                 ++index) {
                DtcSection& section{state_.dtcs[index]};
                section.kind = kDtcKinds[index];
                section.status = controller_.read_dtcs(section.kind, section.codes);
                if (section.status == common::Status::kInterrupted) {
                    status = section.status;
                }
            }
            state_.dtcs_read = is_ok(status) && controller_.connected();
        }
        if (!is_ok(status)) {
            fail("Reading the trouble codes", status);
        } else if (state_.dtcs_read) {
            MessageText text{};
            put(text, "Trouble codes read: ");
            for (std::size_t index{0U}; index < state_.dtcs.size(); ++index) {
                put(text, (index > 0U) ? ", " : "");
                static_cast<void>(common::append_unsigned(text, state_.dtcs[index].codes.size()));
                put(text, " ");
                put(text, std::string_view{elm327::to_string(state_.dtcs[index].kind)});
            }
            put(text, ".");
            note(text.text(), false);
        } else {
            // The device is gone: finish() tells.
        }
        finish();
    }
}

void Worker::read_vin() noexcept {
    if (require_connection()) {
        begin(Activity::kReadingVin);
        common::Status status{controller_.connect_vehicle()};
        if (is_ok(status)) {
            status = controller_.read_vin(state_.vin);
            state_.vin_status = status;
            state_.vin_read = status != common::Status::kInterrupted;
        }
        if (!is_ok(status)) {
            fail("Reading the VIN", status);
        }
        finish();
    }
}

void Worker::send(const Request& request) noexcept {
    if (require_connection()) {
        begin(Activity::kSendingCommand);
        const common::Status status{controller_.execute(request.command.view(), response_)};
        if (!is_ok(status)) {
            const Explanation explanation{explain_failure(status, response_)};
            MessageText text{};
            put(text, request.command.text());
            put(text, ": ");
            put(text, explanation.reason);
            note(text.text(), true);
        }
        finish();
    }
}

common::Status Worker::query(const std::string_view command, AdapterText& text) noexcept {
    common::Status status{controller_.query(command, text)};
    if (status == common::Status::kNotSupported) {
        // Clones often answer "?" to the commands they leave out, e.g. AT@1.
        static_cast<void>(text.assign("unknown"));
        status = common::Status::kOk;
    }
    return status;
}

bool Worker::require_connection() noexcept {
    const bool connected{controller_.connected()};
    if (!connected) {
        note(span_of("Not connected: connect to the adapter first."), true);
        publish();
    }
    return connected;
}

void Worker::forget() noexcept {
    state_.connected = false;
    state_.identity.clear();
    state_.description.clear();
    state_.voltage.clear();
    state_.protocol.clear();
    state_.pids_known = false;
    state_.supported_pids = common::ByteSet{};
    state_.monitoring = false;
    state_.monitored_pids = common::ByteSet{};
    state_.dtcs_read = false;
    state_.mil_known = false;
    state_.vin_read = false;
    state_.vin.clear();
}

void Worker::begin(const Activity activity) noexcept {
    // A cancel() before this operation was meant for an earlier one.
    drain(cancel_descriptor_);
    state_.activity = activity;
    publish();
}

void Worker::finish() noexcept {
    if (state_.connected && (!controller_.connected())) {
        forget();
        note(span_of("The connection to the adapter was lost."), true);
    }
    state_.activity = state_.monitoring ? Activity::kMonitoring : Activity::kIdle;
    publish();
}

void Worker::fail(const char* const what, const common::Status status) noexcept {
    const Explanation explanation{explain_failure(status, controller_.last_response())};
    MessageText text{};
    put(text, std::string_view{what});
    put(text, ": ");
    put(text, explanation.reason);
    if (!explanation.advice.empty()) {
        put(text, " (");
        put(text, explanation.advice);
        put(text, ")");
    }
    note(text.text(), true);
}

void Worker::note(const common::Span<const char> text, const bool failed) noexcept {
    state_.message.clear();
    put(state_.message, text);
    state_.message_failed = failed;
    ++state_.message_count;
    push_trace(failed ? TraceLine::Kind::kError : TraceLine::Kind::kNote, text);
}

void Worker::push_trace(const TraceLine::Kind kind, const common::Span<const char> text) noexcept {
    TraceLine line{};
    line.kind = kind;
    put(line.text, text);
    if (!trace_.try_push(line)) {
        static_cast<void>(dropped_.fetch_add(1U, std::memory_order_relaxed));
    }
}

void Worker::push_sample(const Sample& sample) noexcept {
    if (!samples_.try_push(sample)) {
        static_cast<void>(dropped_.fetch_add(1U, std::memory_order_relaxed));
    }
    if (notify_ != nullptr) {
        notify_(notify_context_);
    }
}

void Worker::publish() noexcept {
    ++state_.version;
    state_buffer_.write(state_);
    if (notify_ != nullptr) {
        notify_(notify_context_);
    }
}

bool Worker::cancelled(const std::uint32_t generation) const noexcept {
    return generation != generation_.load(std::memory_order_acquire);
}

}  // namespace gui
}  // namespace elm327
