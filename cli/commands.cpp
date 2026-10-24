#include "cli/commands.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "cli/options.hpp"
#include "cli/signals.hpp"
#include "common/core/status.hpp"
#include "common/numeric/decimal.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/obd/client.hpp"
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
/// otherwise the description of @p status.
void append_reason(Line& line, const common::Status status, const Response& response) noexcept {
    if (status == common::Status::kInterrupted) {
        line.text("interrupted");
    } else if (is_error(response.reply())) {
        line.text(response.reply_line());
        if (response.reply() == Reply::kUnableToConnect) {
            line.text(" (is the ignition on?)");
        }
    } else if (status == common::Status::kInvalidArgument) {
        line.text("unexpected reply");  // A reply that is not OBD data, or malformed.
    } else {
        line.text(common::to_string(status));
    }
}

/// Appends the value in @p data of the PID @p info: the measurement, and its unit with
/// @p with_unit. A PID without a single value, or one this program does not know, shows its data
/// bytes, separated by @p separator ('\0' for none).
void append_value(Line& line, const common::Result<PidInfo>& info, const common::Span<const std::uint8_t> data,
                  const bool with_unit, const char separator) noexcept {
    const common::Result<Measurement> value{info.has_value()
                                                ? decode_pid(info.value(), data)
                                                : common::Result<Measurement>::failure(common::Status::kNotFound)};
    std::array<char, Measurement::kMaxFormattedSize> text{};
    const common::Result<std::size_t> length{
        value.has_value() ? format_measurement(value.value(), info.value().decimals, text)
                          : common::Result<std::size_t>::failure(value.status())};
    if (length.has_value()) {
        line.text(common::Span<const char>{text.data(), length.value()});
        if (with_unit && (!info.value().unit.empty())) {
            line.character(' ').text(info.value().unit);
        }
    } else {
        for (std::size_t index{0U}; index < data.size(); ++index) {
            if ((index > 0U) && (separator != '\0')) {
                line.character(separator);
            }
            line.hex(data[index], 2U);
        }
    }
}

/// Appends @p duration in seconds with three decimals, e.g. "12.345".
void append_seconds(Line& line, const common::Duration duration) noexcept {
    const common::Decimal<3U> seconds{common::Decimal<3U>::from_units(duration.count() / 1'000'000)};
    std::array<char, common::Decimal<3U>::kMaxFormattedSize> text{};
    const common::Result<std::size_t> length{seconds.format(text)};
    line.text(common::Span<const char>{text.data(), length.value_or(0U)});
}

/// The activation after @p previous on the grid previous + k * interval that has not passed at
/// @p now: activations missed during slow reads are skipped instead of caught up in a burst.
common::TimePoint next_activation(const common::TimePoint previous, const common::Duration interval,
                                  const common::TimePoint now) noexcept {
    common::TimePoint next{previous + interval};
    if (next <= now) {
        const std::int64_t missed{(now - next) / interval};
        next += interval * (missed + 1);
    }
    return next;
}

/// The PID of argument @p index; parse_options() checked it.
std::uint8_t pid_argument(const Options& options, const std::size_t index) noexcept {
    return parse_pid(options.arguments[index]).value_or(0U);
}

/// One run of a command that needs the adapter.
class Session final {
public:
    Session(const Options& options, Console& out, Console& err, const std::int32_t cancel_descriptor) noexcept
        : options_{&options}, out_{&out}, err_{&err}, cancel_descriptor_{cancel_descriptor}, obd_{adapter_} {}

    /// Opens and initializes the adapter, runs the command and closes the adapter.
    /// @return the exit code.
    std::int32_t run() noexcept;

private:
    std::int32_t dispatch() noexcept;
    std::int32_t run_info() noexcept;
    std::int32_t run_pids() noexcept;
    std::int32_t run_read() noexcept;
    std::int32_t run_monitor() noexcept;
    std::int32_t run_dtc() noexcept;
    std::int32_t run_vin() noexcept;
    std::int32_t run_raw() noexcept;

    /// Prints "elm327ctl: <what>: <reason>", see append_reason(). @return the exit code for @p status.
    std::int32_t fail(std::string_view what, common::Status status, const Response& response) noexcept;

    /// Explains why the device could not be opened.
    void report_open_error(common::Status status) noexcept;

    /// Prints the reply to the AT command @p command, a line of text such as "12.6V", under
    /// @p label; "unknown" if the adapter does not know the command. @return the status of execute().
    common::Status print_query(std::string_view label, std::string_view command) noexcept;

    void print_field(std::string_view label, common::Span<const char> value) noexcept;

    const Options* options_;
    Console* out_;
    Console* err_;
    std::int32_t cancel_descriptor_;
    Adapter adapter_{};
    ObdClient obd_;
    Response response_{};  ///< Replies to AT commands and raw commands.
};

std::int32_t Session::run() noexcept {
    adapter_.set_cancel_descriptor(cancel_descriptor_);
    if (options_->verbose) {
        adapter_.set_trace(&trace_to_console, err_);
    }
    AdapterOptions adapter_options{};
    adapter_options.serial.baud_rate = options_->baud_rate;
    adapter_options.timeouts.command = options_->timeout;
    std::int32_t exit_code{kExitFailure};
    const common::Status opened{adapter_.open(options_->device, adapter_options)};
    if (!is_ok(opened)) {
        report_open_error(opened);
    } else {
        const common::Status initialized{adapter_.initialize(options_->protocol)};
        if (is_ok(initialized)) {
            exit_code = dispatch();
        } else if (initialized == common::Status::kTimeout) {
            err_->print(Line{}.text(kProgram).text(": the adapter does not answer (check the device, and the baud "
                                                   "rate with -b, e.g. 9600 or 115200)"));
        } else {
            exit_code = fail("initializing the adapter", initialized, response_);
        }
        // The port is closed in any case; an error here would change nothing for the user.
        static_cast<void>(adapter_.close());
    }
    return exit_code;
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
        case Command::kHelp:
        case Command::kVersion:
            break;  // Handled without the adapter.
    }
    return exit_code;
}

std::int32_t Session::run_info() noexcept {
    print_field("Adapter", adapter_.identity());
    common::Status status{print_query("Description", "AT@1")};
    if (is_ok(status)) {
        status = print_query("Voltage", "ATRV");
    }
    std::int32_t exit_code{is_ok(status) ? kExitSuccess : fail("querying the adapter", status, response_)};
    if (is_ok(status)) {
        // The first OBD request makes an adapter in automatic mode find the vehicle's protocol.
        status = obd_.connect();
        exit_code = is_ok(status) ? kExitSuccess : fail("connecting to the vehicle", status, obd_.response());
    }
    if (is_ok(status)) {
        status = print_query("Protocol", "ATDP");
        exit_code = is_ok(status) ? kExitSuccess : fail("querying the adapter", status, response_);
    }
    return exit_code;
}

std::int32_t Session::run_pids() noexcept {
    const common::Status status{obd_.read_supported_pids()};
    std::int32_t exit_code{kExitSuccess};
    if (!is_ok(status)) {
        exit_code = fail("reading the supported PIDs", status, obd_.response());
    } else {
        std::uint32_t listed{0U};
        for (std::uint32_t value{1U}; value <= 0xFFU; ++value) {
            const std::uint8_t pid{static_cast<std::uint8_t>(value)};
            // The support PIDs 20, 40, ... only link the ranges.
            if (obd_.supported_pids().contains(pid) && (!is_support_pid(pid))) {
                const common::Result<PidInfo> info{find_pid(pid)};
                Line line{};
                line.hex(pid, 2U).pad(2U + kGap).text(info.has_value() ? info.value().name : kUnknownPid);
                if (info.has_value() && (!info.value().unit.empty())) {
                    line.text(" [").text(info.value().unit).character(']');
                }
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
    const common::Status connected{obd_.read_supported_pids()};
    std::int32_t exit_code{is_ok(connected) ? kExitSuccess
                                            : fail("connecting to the vehicle", connected, obd_.response())};
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
        const common::Result<PidInfo> info{find_pid(pid)};
        Line line{};
        line.hex(pid, 2U).pad(2U + kGap).text(info.has_value() ? info.value().name : kUnknownPid);
        line.pad(2U + kGap + name_width + kGap);
        if (!obd_.supported_pids().contains(pid)) {
            line.text("not supported by the vehicle");
            exit_code = kExitFailure;
        } else {
            const common::Result<common::Span<const std::uint8_t>> data{obd_.read_pid(pid)};
            if (data.has_value()) {
                append_value(line, info, data.value(), true, ' ');
            } else if (data.status() == common::Status::kInterrupted) {
                status = data.status();
            } else {
                append_reason(line, data.status(), obd_.response());
                exit_code = kExitFailure;
            }
        }
        if (is_ok(status)) {
            out_->print(line);
        }
    }
    if (is_ok(connected) && (status == common::Status::kInterrupted)) {
        exit_code = fail("reading the PIDs", status, obd_.response());
    }
    return exit_code;
}

std::int32_t Session::run_monitor() noexcept {
    common::Status status{obd_.read_supported_pids()};
    std::int32_t exit_code{is_ok(status) ? kExitSuccess : fail("connecting to the vehicle", status, obd_.response())};
    const std::size_t pid_count{options_->arguments.size()};
    Line header{};
    if (is_ok(status)) {
        // A legend of comment lines, then a table with one row per sample.
        for (std::size_t index{0U}; index < pid_count; ++index) {
            const std::uint8_t pid{pid_argument(*options_, index)};
            const common::Result<PidInfo> info{find_pid(pid)};
            Line legend{};
            legend.text("# ").hex(pid, 2U).pad(2U + 2U + kGap).text(info.has_value() ? info.value().name : kUnknownPid);
            if (info.has_value() && (!info.value().unit.empty())) {
                legend.text(" [").text(info.value().unit).character(']');
            }
            if (!obd_.supported_pids().contains(pid)) {
                legend.text(" (not supported by the vehicle)");
            }
            out_->print(legend);
        }
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
            if (!obd_.supported_pids().contains(pid)) {
                cell.text("--");
            } else {
                const common::Result<common::Span<const std::uint8_t>> data{obd_.read_pid(pid)};
                if (data.has_value()) {
                    append_value(cell, find_pid(pid), data.value(), false, '\0');
                } else if (data.status() == common::Status::kInterrupted) {
                    status = data.status();
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
    const common::Status connected{obd_.connect()};
    std::int32_t exit_code{is_ok(connected) ? kExitSuccess
                                            : fail("connecting to the vehicle", connected, obd_.response())};
    common::Status status{connected};
    if (is_ok(status) && obd_.supported_pids().contains(kMilStatusPid)) {
        const common::Result<common::Span<const std::uint8_t>> data{obd_.read_pid(kMilStatusPid)};
        const common::Result<MilStatus> mil{data.has_value() ? decode_mil_status(data.value())
                                                             : common::Result<MilStatus>::failure(data.status())};
        if (mil.has_value()) {
            const std::uint8_t count{mil.value().dtc_count};
            out_->print(Line{}
                            .text("MIL")
                            .pad(kLabelWidth)
                            .text(mil.value().on ? "on, " : "off, ")
                            .number(count)
                            .text((count == 1U) ? " confirmed DTC" : " confirmed DTCs"));
        } else if (mil.status() == common::Status::kInterrupted) {
            status = mil.status();
        } else {
            // Without the MIL status, the lists below still tell everything.
        }
    }
    for (std::size_t section{0U}; is_ok(status) && (section < kDtcSections.size()); ++section) {
        DtcList dtcs{};
        const common::Status read{obd_.read_dtcs(kDtcSections[section].kind, dtcs)};
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
            append_reason(line, read, obd_.response());
            exit_code = kExitFailure;
        }
        if (is_ok(status)) {
            out_->print(line);
        }
    }
    if (is_ok(connected) && (status == common::Status::kInterrupted)) {
        exit_code = fail("reading the DTCs", status, obd_.response());
    }
    return exit_code;
}

std::int32_t Session::run_vin() noexcept {
    common::Status status{obd_.connect()};
    std::int32_t exit_code{is_ok(status) ? kExitSuccess : fail("connecting to the vehicle", status, obd_.response())};
    if (is_ok(status)) {
        Vin vin{};
        status = obd_.read_vin(vin);
        if (is_ok(status)) {
            out_->print(Line{}.text(vin.text()));
        } else {
            exit_code = fail("reading the VIN", status, obd_.response());
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
        // An OBD request may make the adapter search for the vehicle's protocol first.
        status = adapter_.execute(view_of(command), response_, common::Deadline::after(adapter_.timeouts().connect));
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

void Session::report_open_error(const common::Status status) noexcept {
    Line line{};
    line.text(kProgram).text(": ").text(options_->device).text(": ");
    if (status == common::Status::kNotFound) {
        line.text("no such device (choose the adapter's serial device with -d)");
    } else if (status == common::Status::kPermissionDenied) {
        line.text("permission denied (add your user to the dialout group)");
    } else if (status == common::Status::kResourceLimit) {
        line.text("in use by another program");
    } else if (status == common::Status::kInvalidArgument) {
        line.text("not a serial device");
    } else if (status == common::Status::kNotSupported) {
        line.text("the device does not accept the line settings");
    } else {
        line.text(common::to_string(status));
    }
    err_->print(line);
}

common::Status Session::print_query(const std::string_view label, const std::string_view command) noexcept {
    const common::Status status{adapter_.execute(command, response_)};
    if (is_ok(status)) {
        // Clones often answer "?" to the commands they leave out, e.g. AT@1.
        print_field(label, (response_.reply() == Reply::kContent) ? response_.reply_line() : span_of("unknown"));
    }
    return status;
}

void Session::print_field(const std::string_view label, const common::Span<const char> value) noexcept {
    out_->print(Line{}.text(label).pad(kLabelWidth).text(value));
}

}  // namespace

std::int32_t run(const common::Span<const char* const> args, Console& out, Console& err,
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
    } else {
        Session session{options, out, err, cancel_descriptor};
        exit_code = session.run();
    }
    return exit_code;
}

}  // namespace cli
}  // namespace elm327
