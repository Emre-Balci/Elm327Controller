#include "cli/commands.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "cli/shell.hpp"
#include "common/core/status.hpp"
#include "common/numeric/decimal.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/controller/explain.hpp"
#include "elm327/controller/timing.hpp"
#include "elm327/obd/dtc.hpp"
#include "elm327/obd/pid.hpp"
#include "elm327/obd/vin.hpp"
#include "elm327/version.hpp"

namespace elm327 {
namespace cli {
namespace {

constexpr std::string_view kProgram{"elm327ctl"};
constexpr std::string_view kUnknownPid{"(unknown PID)"};

/// Column of the values in the label-value output of info and dtc.
constexpr std::size_t kLabelWidth{13U};

/// read: space between the PID and its name, and between the name and the value.
constexpr std::size_t kGap{2U};

/// monitor: width of the time column and of each value column.
constexpr std::size_t kTimeWidth{10U};
constexpr std::size_t kValueWidth{10U};

constexpr std::uint8_t kMilStatusPid{0x01U};

struct DtcSection final {
    DtcKind kind;
    std::string_view label;
};

constexpr std::array<DtcSection, 3U> kDtcSections{{
    {DtcKind::kStored, "Stored"},
    {DtcKind::kPending, "Pending"},
    {DtcKind::kPermanent, "Permanent"},
}};

/// The advice of the messages about the device and the baud rate, in the words of each Style.
struct Advice final {
    std::string_view device;
    std::string_view baud;
};

constexpr Advice kCommandLineAdvice{"choose the adapter's serial device with -d", "-b"};
constexpr Advice kShellAdvice{"choose the adapter's serial device with 'set device' or 'connect'", "'set baud'"};

std::string_view view_of(const common::Span<const char> text) noexcept {
    return std::string_view{text.data(), text.size()};
}

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

/// Prints the traffic with the adapter on the Console passed as @p context (--verbose).
void trace_to_console(const TraceEvent event, const common::Span<const char> text, void* const context) noexcept {
    Console* const console{static_cast<Console*>(context)};
    console->print(Line{}.text((event == TraceEvent::kCommand) ? "-> " : "<- ").text(text));
}

/// Appends why an operation failed: the adapter's own message when it sent one, e.g. "NO DATA",
/// otherwise the description of @p status; see explain_failure().
void append_reason(Line& line, const common::Status status, const Response& response) noexcept {
    const Explanation explanation{explain_failure(status, response)};
    line.text(explanation.reason);
    if (!explanation.advice.empty()) {
        line.text(" (").text(explanation.advice).character(')');
    }
}

/// Appends the value of @p reading, see format_reading().
void append_value(Line& line, const PidReading& reading, const bool with_unit, const char separator) noexcept {
    std::array<char, kMaxReadingText> text{};
    const common::Result<std::size_t> length{format_reading(reading, with_unit, separator, text)};
    line.text(common::Span<const char>{text.data(), length.value_or(0U)});
}

/// Appends @p duration in seconds with three decimals, e.g. "12.345".
void append_seconds(Line& line, const common::Duration duration) noexcept {
    const common::Decimal<3U> seconds{common::Decimal<3U>::from_units(duration.count() / 1'000'000)};
    std::array<char, common::Decimal<3U>::kMaxFormattedSize> text{};
    const common::Result<std::size_t> length{seconds.format(text)};
    line.text(common::Span<const char>{text.data(), length.value_or(0U)});
}

/// Appends the name of @p pid, and its unit in brackets with @p with_unit, e.g. "Engine speed [rpm]".
void append_name(Line& line, const std::uint8_t pid, const bool with_unit) noexcept {
    const common::Result<PidInfo> info{find_pid(pid)};
    line.text(info.has_value() ? info.value().name : kUnknownPid);
    if (with_unit && info.has_value() && (!info.value().unit.empty())) {
        line.text(" [").text(info.value().unit).character(']');
    }
}

/// The PID of argument @p index; parse_options() checked it.
std::uint8_t pid_argument(const Options& options, const std::size_t index) noexcept {
    return parse_pid(options.arguments[index]).value_or(0U);
}

}  // namespace

Session::Session(Console& out, Console& err, const std::int32_t cancel_descriptor, const Style style) noexcept
    : out_{&out}, err_{&err}, cancel_descriptor_{cancel_descriptor}, style_{style} {
    controller_.set_cancel_descriptor(cancel_descriptor);
}

std::int32_t Session::run(const Options& options) noexcept {
    options_ = &options;
    controller_.set_trace(options.verbose ? &trace_to_console : nullptr, err_);
    std::int32_t exit_code{kExitSuccess};
    if (options.command == Command::kConnect) {
        exit_code = connect(*out_);
    } else if (options.command == Command::kDisconnect) {
        exit_code = run_disconnect();
    } else {
        if (needs_adapter(options.command) && (!controller_.connected())) {
            // The shell tells that it connected; the command line connects for every command anyway.
            exit_code = connect(*err_);
        }
        if (exit_code == kExitSuccess) {
            exit_code = dispatch();
        }
    }
    options_ = nullptr;
    return exit_code;
}

std::int32_t Session::connect(Console& notices) noexcept {
    const common::Status status{controller_.connect(options_->connection)};
    std::int32_t exit_code{kExitSuccess};
    if (!is_ok(status)) {
        exit_code = report_connect_failure(status);
    } else if (style_ == Style::kShell) {
        const ConnectionSettings& settings{controller_.settings()};
        notices.print(Line{}
                          .text("Connected to ")
                          .text(controller_.identity())
                          .text(" on ")
                          .text(settings.device.text())
                          .text(" at ")
                          .number(static_cast<std::uint32_t>(settings.baud_rate))
                          .text(" bit/s."));
    } else {
        // Silent: the command's output follows.
    }
    return exit_code;
}

std::int32_t Session::run_disconnect() noexcept {
    if (controller_.connected()) {
        controller_.disconnect();
        out_->print(Line{}.text("Disconnected from ").text(controller_.settings().device.text()).character('.'));
    } else {
        out_->print("Not connected.");
    }
    return kExitSuccess;
}

std::int32_t Session::dispatch() noexcept {
    std::int32_t exit_code{kExitFailure};
    switch (options_->command) {
        case Command::kInfo:
            exit_code = run_info();
            break;
        case Command::kPids:
            exit_code = run_pids();
            break;
        case Command::kRead:
            exit_code = run_read();
            break;
        case Command::kMonitor:
            exit_code = run_monitor();
            break;
        case Command::kDtc:
            exit_code = run_dtc();
            break;
        case Command::kVin:
            exit_code = run_vin();
            break;
        case Command::kRaw:
            exit_code = run_raw();
            break;
        case Command::kShell:
        case Command::kNone:
        case Command::kHelp:
        case Command::kVersion:
        case Command::kConnect:
        case Command::kDisconnect:
        case Command::kSet:
        case Command::kShow:
        case Command::kQuit:
            break;  // Handled without the adapter, by run() or the shell.
    }
    return exit_code;
}

std::int32_t Session::run_info() noexcept {
    print_field("Adapter", controller_.identity());
    common::Status status{print_query("Description", "AT@1")};
    if (is_ok(status)) {
        status = print_query("Voltage", "ATRV");
    }
    std::int32_t exit_code{is_ok(status) ? kExitSuccess
                                         : fail("querying the adapter", status, controller_.last_response())};
    if (is_ok(status)) {
        // The first OBD request makes an adapter in automatic mode find the vehicle's protocol.
        status = controller_.connect_vehicle();
        exit_code = is_ok(status) ? kExitSuccess
                                  : fail("connecting to the vehicle", status, controller_.last_response());
    }
    if (is_ok(status)) {
        status = print_query("Protocol", "ATDP");
        exit_code = is_ok(status) ? kExitSuccess : fail("querying the adapter", status, controller_.last_response());
    }
    return exit_code;
}

std::int32_t Session::run_pids() noexcept {
    const common::Status status{controller_.read_supported_pids()};
    std::int32_t exit_code{kExitSuccess};
    if (!is_ok(status)) {
        exit_code = fail("reading the supported PIDs", status, controller_.last_response());
    } else {
        std::uint32_t listed{0U};
        for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
            const std::uint8_t pid{static_cast<std::uint8_t>(value)};
            // The support PIDs 20, 40, ... only link the ranges.
            if (controller_.supported_pids().contains(pid) && (!is_support_pid(pid))) {
                Line line{};
                line.hex(pid, 2U).pad(2U + kGap);
                append_name(line, pid, true);
                out_->print(line);
                ++listed;
            }
        }
        if (listed == 0U) {
            out_->print("The vehicle supports no service 01 PIDs.");
        }
    }
    return exit_code;
}

std::int32_t Session::run_read() noexcept {
    const common::Status connected{controller_.ensure_supported_pids()};
    std::int32_t exit_code{is_ok(connected) ? kExitSuccess
                                            : fail("connecting to the vehicle", connected,
                                                   controller_.last_response())};
    common::Status status{connected};
    // The values start in one column, after the longest name.
    std::size_t name_width{kUnknownPid.size()};
    for (std::size_t index{0U}; index < options_->arguments.size(); ++index) {
        const common::Result<PidInfo> info{find_pid(pid_argument(*options_, index))};
        if (info.has_value() && (info.value().name.size() > name_width)) {
            name_width = info.value().name.size();
        }
    }
    for (std::size_t index{0U}; is_ok(status) && (index < options_->arguments.size()); ++index) {
        const std::uint8_t pid{pid_argument(*options_, index)};
        Line line{};
        line.hex(pid, 2U).pad(2U + kGap);
        append_name(line, pid, false);
        line.pad(2U + kGap + name_width + kGap);
        bool print{true};
        if (!controller_.supported_pids().contains(pid)) {
            line.text("not supported by the vehicle");
            exit_code = kExitFailure;
        } else {
            const PidReading reading{controller_.read_pid(pid)};
            if (is_ok(reading.status)) {
                append_value(line, reading, true, ' ');
            } else if (reading.status == common::Status::kInterrupted) {
                status = reading.status;
                print = false;
            } else {
                append_reason(line, reading.status, controller_.last_response());
                exit_code = kExitFailure;
                if (!controller_.connected()) {
                    status = reading.status;  // The device is gone: the other PIDs would fail as well.
                }
            }
        }
        if (print) {
            out_->print(line);
        }
    }
    if (is_ok(connected) && (status == common::Status::kInterrupted)) {
        exit_code = fail("reading the PIDs", status, controller_.last_response());
    }
    return exit_code;
}

std::int32_t Session::run_monitor() noexcept {
    common::Status status{controller_.ensure_supported_pids()};
    std::int32_t exit_code{is_ok(status) ? kExitSuccess
                                         : fail("connecting to the vehicle", status, controller_.last_response())};
    const std::size_t pid_count{options_->arguments.size()};
    if (is_ok(status)) {
        // A legend of comment lines, then a table with one row per sample.
        for (std::size_t index{0U}; index < pid_count; ++index) {
            const std::uint8_t pid{pid_argument(*options_, index)};
            Line legend{};
            legend.text("# ").hex(pid, 2U).pad(2U + 2U + kGap);
            append_name(legend, pid, true);
            if (!controller_.supported_pids().contains(pid)) {
                legend.text(" (not supported by the vehicle)");
            }
            out_->print(legend);
        }
        Line header{};
        header.right(span_of("time [s]"), kTimeWidth);
        for (std::size_t index{0U}; index < pid_count; ++index) {
            Line cell{};
            cell.hex(pid_argument(*options_, index), 2U);
            header.right(cell.view(), kValueWidth);
        }
        out_->print(header);
    }
    const common::TimePoint start{common::MonotonicClock::now()};
    common::TimePoint next{start};
    const std::uint32_t samples{(options_->count > 0U) ? options_->count : std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t printed{0U};
    std::uint32_t failed_reads{0U};
    for (std::uint32_t sample{0U}; is_ok(status) && (sample < samples); ++sample) {
        Line row{};
        Line time{};
        append_seconds(time, common::MonotonicClock::now() - start);
        row.right(time.view(), kTimeWidth);
        for (std::size_t index{0U}; is_ok(status) && (index < pid_count); ++index) {
            const std::uint8_t pid{pid_argument(*options_, index)};
            Line cell{};
            if (!controller_.supported_pids().contains(pid)) {
                cell.text("--");
            } else {
                const PidReading reading{controller_.read_pid(pid)};
                if (is_ok(reading.status)) {
                    append_value(cell, reading, false, '\0');
                } else if ((reading.status == common::Status::kInterrupted) || (!controller_.connected())) {
                    status = reading.status;
                } else {
                    cell.text("--");
                    ++failed_reads;
                }
            }
            row.right(cell.view(), kValueWidth);
        }
        if (is_ok(status)) {
            out_->print(row);
            if (out_->failed()) {
                status = common::Status::kSystemError;  // Nobody reads the output any more, e.g. after "| head".
            } else {
                ++printed;
            }
        }
        if (is_ok(status) && ((sample + 1U) < samples)) {
            next = next_activation(next, options_->interval, common::MonotonicClock::now());
            status = wait_until(cancel_descriptor_, common::Deadline::at(next));
        }
    }
    if (exit_code == kExitSuccess) {
        if (status == common::Status::kDisconnected) {
            exit_code = fail("reading the PIDs", status, controller_.last_response());
        }
        Line summary{};
        summary.text(kProgram).text(": ").number(printed).text(" samples, ").number(failed_reads).text(" failed reads");
        err_->print(summary);
        if (status == common::Status::kInterrupted) {
            exit_code = kExitInterrupted;
        } else {
            exit_code = (is_ok(status) && (failed_reads == 0U)) ? kExitSuccess : kExitFailure;
        }
    }
    return exit_code;
}

std::int32_t Session::run_dtc() noexcept {
    const common::Status connected{controller_.connect_vehicle()};
    std::int32_t exit_code{is_ok(connected) ? kExitSuccess
                                            : fail("connecting to the vehicle", connected,
                                                   controller_.last_response())};
    common::Status status{connected};
    if (is_ok(status) && controller_.supported_pids().contains(kMilStatusPid)) {
        const common::Result<MilStatus> mil{controller_.read_mil_status()};
        if (mil.has_value()) {
            const std::uint8_t count{mil.value().dtc_count};
            out_->print(Line{}
                            .text("MIL")
                            .pad(kLabelWidth)
                            .text(mil.value().on ? "on, " : "off, ")
                            .number(count)
                            .text((count == 1U) ? " confirmed DTC" : " confirmed DTCs"));
        } else if ((mil.status() == common::Status::kInterrupted) || (!controller_.connected())) {
            status = mil.status();
        } else {
            // Without the MIL status, the lists below still tell everything.
        }
    }
    for (std::size_t section{0U}; is_ok(status) && (section < kDtcSections.size()); ++section) {
        DtcList dtcs{};
        const common::Status read{controller_.read_dtcs(kDtcSections[section].kind, dtcs)};
        Line line{};
        line.text(kDtcSections[section].label).pad(kLabelWidth);
        if (is_ok(read)) {
            if (dtcs.empty()) {
                line.text("none");
            }
            for (std::size_t index{0U}; index < dtcs.size(); ++index) {
                std::array<char, kDtcTextSize> text{};
                const common::Result<std::size_t> length{format_dtc(dtcs[index], text)};
                line.text((index > 0U) ? " " : "").text(common::Span<const char>{text.data(), length.value_or(0U)});
            }
        } else if (read == common::Status::kInterrupted) {
            status = read;
        } else if (read == common::Status::kNotFound) {
            line.text("no answer");  // Some ECUs stay silent when they have no codes of the kind.
        } else if (read == common::Status::kNotSupported) {
            line.text("not supported");
        } else {
            append_reason(line, read, controller_.last_response());
            exit_code = kExitFailure;
            if (!controller_.connected()) {
                status = read;  // The device is gone.
                out_->print(line);
            }
        }
        if (is_ok(status)) {
            out_->print(line);
        }
    }
    if (is_ok(connected) && (status == common::Status::kInterrupted)) {
        exit_code = fail("reading the DTCs", status, controller_.last_response());
    }
    return exit_code;
}

std::int32_t Session::run_vin() noexcept {
    common::Status status{controller_.connect_vehicle()};
    std::int32_t exit_code{is_ok(status) ? kExitSuccess
                                         : fail("connecting to the vehicle", status, controller_.last_response())};
    if (is_ok(status)) {
        Vin vin{};
        status = controller_.read_vin(vin);
        if (is_ok(status)) {
            out_->print(Line{}.text(vin.text()));
        } else {
            exit_code = fail("reading the VIN", status, controller_.last_response());
        }
    }
    return exit_code;
}

std::int32_t Session::run_raw() noexcept {
    std::int32_t exit_code{kExitSuccess};
    common::Status status{common::Status::kOk};
    const bool several{options_->arguments.size() > 1U};
    for (std::size_t index{0U}; is_ok(status) && (index < options_->arguments.size()); ++index) {
        const common::Span<const char> command{options_->arguments[index]};
        if (several) {
            out_->print(Line{}.text("> ").text(command));
        }
        status = controller_.execute(view_of(command), response_);
        for (std::size_t row{0U}; row < response_.size(); ++row) {
            out_->print(Line{}.text(response_.line(row)));
        }
        if (!is_ok(status)) {
            exit_code = fail(view_of(command), status, response_);
        } else if (is_error(response_.reply())) {
            exit_code = kExitFailure;  // The adapter's message is printed above.
        } else {
            // Answered.
        }
    }
    return exit_code;
}

std::int32_t Session::fail(const std::string_view what, const common::Status status,
                           const Response& response) noexcept {
    Line line{};
    line.text(kProgram).text(": ").text(what).text(": ");
    append_reason(line, status, response);
    err_->print(line);
    return (status == common::Status::kInterrupted) ? kExitInterrupted : kExitFailure;
}

std::int32_t Session::report_connect_failure(const common::Status status) noexcept {
    const Advice& advice{(style_ == Style::kShell) ? kShellAdvice : kCommandLineAdvice};
    std::int32_t exit_code{kExitFailure};
    switch (controller_.connect_failure()) {
        case ConnectFailure::kOpen: {
            const Explanation explanation{explain_open_failure(status)};
            Line line{};
            line.text(kProgram).text(": ").text(controller_.settings().device.text()).text(": ");
            line.text(explanation.reason);
            const std::string_view hint{(status == common::Status::kNotFound) ? advice.device : explanation.advice};
            if (!hint.empty()) {
                line.text(" (").text(hint).character(')');
            }
            err_->print(line);
            break;
        }
        case ConnectFailure::kNoAnswer:
            err_->print(Line{}
                            .text(kProgram)
                            .text(": the adapter does not answer (check the device, and the baud rate with ")
                            .text(advice.baud)
                            .text(", e.g. 9600 or 115200)"));
            break;
        case ConnectFailure::kNone:
        case ConnectFailure::kInitialize:
            exit_code = fail("initializing the adapter", status, controller_.last_response());
            break;
    }
    return exit_code;
}

common::Status Session::print_query(const std::string_view label, const std::string_view command) noexcept {
    AdapterText text{};
    common::Status status{controller_.query(command, text)};
    if (status == common::Status::kNotSupported) {
        // Clones often answer "?" to the commands they leave out, e.g. AT@1.
        print_field(label, span_of("unknown"));
        status = common::Status::kOk;
    } else if (is_ok(status)) {
        print_field(label, text.text());
    } else {
        // Reported by the caller.
    }
    return status;
}

void Session::print_field(const std::string_view label, const common::Span<const char> value) noexcept {
    out_->print(Line{}.text(label).pad(kLabelWidth).text(value));
}

std::int32_t run(const common::Span<const char* const> args, const std::int32_t input, Console& out, Console& err,
                 const std::int32_t cancel_descriptor) noexcept {
    Options options{};
    Line error{};
    std::int32_t exit_code{kExitSuccess};
    if (!is_ok(parse_options(args, options, error))) {
        err.print(Line{}.text(kProgram).text(": ").text(error.view()));
        err.print(Line{}.text("Try '").text(kProgram).text(" --help' for more information."));
        exit_code = kExitUsage;
    } else if (options.command == Command::kHelp) {
        print_usage(out);
    } else if (options.command == Command::kVersion) {
        out.print(Line{}.text(kProgram).character(' ').text(kVersion));
    } else if (options.command == Command::kShell) {
        exit_code = run_shell(options, input, out, err, cancel_descriptor);
    } else {
        Session session{out, err, cancel_descriptor, Style::kCommandLine};
        exit_code = session.run(options);
        session.disconnect();
    }
    return exit_code;
}

}  // namespace cli
}  // namespace elm327
