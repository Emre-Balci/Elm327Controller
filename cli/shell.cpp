#include "cli/shell.hpp"

#include <signal.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "cli/commands.hpp"
#include "cli/line_editor.hpp"
#include "cli/terminal.hpp"
#include "common/core/status.hpp"
#include "common/protocol/text.hpp"
#include "elm327/adapter/adapter.hpp"
#include "elm327/controller/controller.hpp"
#include "elm327/controller/signals.hpp"
#include "elm327/version.hpp"

namespace elm327 {
namespace cli {
namespace {

constexpr std::string_view kProgram{"elm327ctl"};
constexpr std::string_view kPrompt{"elm327> "};

/// Lines the shell reads at most: a bound for the loop, not a limit anybody reaches.
constexpr std::uint64_t kMaxLines{std::numeric_limits<std::uint64_t>::max()};

/// Column of the values in the output of show.
constexpr std::size_t kLabelWidth{13U};

/// Completions of the values of 'set verbose' and 'set protocol'.
constexpr std::array<std::string_view, 2U> kVerboseValues{{"on", "off"}};
constexpr std::array<std::string_view, 13U> kProtocolValues{{
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C",
}};

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

/// Completes the commands as the first word, the settings after 'set', and the values of some
/// settings after them.
void complete(const common::Span<const char> before, LineEditor::Candidates& candidates, void* /*context*/) noexcept {
    Words words{};
    Line error{};
    common::Span<const std::string_view> source{};
    if (is_ok(split_words(before, words, error))) {
        const bool set{(!words.empty()) && common::equals(words[0U], "set")};
        if (words.empty()) {
            source = shell_commands();
        } else if (set && (words.size() == 1U)) {
            source = setting_names();
        } else if (set && (words.size() == 2U) &&
                   (common::equals(words[1U], "verbose") || common::equals(words[1U], "readonly"))) {
            source = common::Span<const std::string_view>{kVerboseValues.data(), kVerboseValues.size()};
        } else if (set && (words.size() == 2U) && common::equals(words[1U], "protocol")) {
            source = common::Span<const std::string_view>{kProtocolValues.data(), kProtocolValues.size()};
        } else {
            // Nothing to complete, e.g. a PID or a device.
        }
    }
    for (const std::string_view candidate : source) {
        if (!candidates.full()) {
            static_cast<void>(candidates.push_back(candidate));  // Cannot fail: the vector is not full.
        }
    }
}

/// @return true if the two settings connect in the same way.
bool same_connection(const ConnectionSettings& first, const ConnectionSettings& second) noexcept {
    return common::equals(first.device.text(), second.device.view()) && (first.baud_rate == second.baud_rate) &&
           (first.protocol == second.protocol) && (first.timeouts.command == second.timeouts.command);
}

std::uint64_t milliseconds(const common::Duration duration) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

/// The interactive shell: reads lines and runs them over one Session.
class Shell final {
public:
    Shell(const Options& options, const common::NativeHandle input, Console& out, Console& err,
          const common::NativeHandle cancel_descriptor) noexcept
        : settings_{options},
          session_{out, err, cancel_descriptor, Style::kShell},
          input_{input, out, cancel_descriptor},
          out_{&out},
          err_{&err},
          cancel_descriptor_{cancel_descriptor} {
        input_.editor().set_completer(&complete, nullptr);
    }

    /// @return the exit code, see run_shell().
    [[nodiscard]] std::int32_t run() noexcept;

private:
    void handle(common::Span<const char> text) noexcept;
    void run_set(const Options& options) noexcept;
    void run_connect(const Options& options) noexcept;
    void show() noexcept;
    void usage_error(const Line& error) noexcept;

    Options settings_;  ///< The settings, which every line starts from.
    Session session_;
    LineInput input_;
    Console* out_;
    Console* err_;
    common::NativeHandle cancel_descriptor_;
    std::int32_t exit_code_{kExitSuccess};  ///< Of the last command.
    std::int32_t stop_signal_{0};           ///< The signal that ended the shell.
    bool done_{false};
};

std::int32_t Shell::run() noexcept {
    if (input_.editing()) {
        out_->print(Line{}.text(kProgram).character(' ').text(kVersion).text(
            ", the shell for ELM327 adapters. Type 'help' for the commands, 'quit' to leave."));
    }
    for (std::uint64_t count{0U}; (!done_) && (count < kMaxLines); ++count) {
        switch (input_.read(kPrompt)) {
            case InputEvent::kLine:
                input_.editor().remember(input_.line());
                handle(input_.line());
                break;
            case InputEvent::kCancelled:
                // At a terminal Ctrl+C only discards the line; a script stops.
                if (!input_.editing()) {
                    stop_signal_ = SIGINT;
                    done_ = true;
                }
                break;
            case InputEvent::kEnd:
                done_ = true;
                break;
            case InputEvent::kStopped:
                stop_signal_ = input_.signal();
                done_ = true;
                break;
            case InputEvent::kTooLong:
                err_->print(Line{}.text(kProgram).text(": line too long, skipped"));
                exit_code_ = kExitUsage;
                break;
            case InputEvent::kFailed:
                err_->print(Line{}.text(kProgram).text(": cannot read the input"));
                exit_code_ = kExitFailure;
                done_ = true;
                break;
        }
        if (!done_) {
            // Ctrl+C has stopped the command already; the other stop signals end the shell.
            const std::int32_t signal{take_stop_request(cancel_descriptor_)};
            if (ends_program(signal)) {
                stop_signal_ = signal;
                done_ = true;
            }
            done_ = done_ || out_->failed();  // Nobody reads the results any more.
        }
    }
    session_.disconnect();
    return (stop_signal_ != 0) ? (128 + stop_signal_) : exit_code_;
}

void Shell::handle(const common::Span<const char> text) noexcept {
    Words words{};
    Line error{};
    Options options{};
    common::Status status{split_words(text, words, error)};
    if (is_ok(status)) {
        status = parse_line(words, settings_, options, error);
    }
    if (!is_ok(status)) {
        usage_error(error);
    } else {
        switch (options.command) {
            case Command::kNone:
            case Command::kShell:
                break;
            case Command::kHelp:
                print_shell_help(*out_);
                exit_code_ = kExitSuccess;
                break;
            case Command::kVersion:
                out_->print(Line{}.text(kProgram).character(' ').text(kVersion));
                exit_code_ = kExitSuccess;
                break;
            case Command::kQuit:
                done_ = true;
                break;
            case Command::kSet:
                run_set(options);
                break;
            case Command::kShow:
                show();
                exit_code_ = kExitSuccess;
                break;
            case Command::kConnect:
                run_connect(options);
                break;
            case Command::kInfo:
            case Command::kPids:
            case Command::kRead:
            case Command::kMonitor:
            case Command::kDtc:
            case Command::kVin:
            case Command::kRaw:
            case Command::kClear:
            case Command::kUds:
            case Command::kCoding:
            case Command::kDisconnect:
                exit_code_ = session_.run(options);
                break;
        }
    }
}

void Shell::run_set(const Options& options) noexcept {
    if (options.arguments.empty()) {
        show();
        exit_code_ = kExitSuccess;
    } else {
        const common::Span<const char> name{options.arguments[0U]};
        Options changed{settings_};
        Line error{};
        if (!is_ok(apply_setting(name, options.arguments[1U], changed, error))) {
            usage_error(error);
        } else {
            const bool reconnect{session_.controller().connected() &&
                                 (!same_connection(changed.connection, settings_.connection))};
            settings_ = changed;
            exit_code_ = kExitSuccess;
            if (reconnect) {
                session_.disconnect();
                err_->print("Disconnected: the next command connects with the new setting.");
            }
        }
    }
}

void Shell::run_connect(const Options& options) noexcept {
    Line error{};
    common::Status status{common::Status::kOk};
    if (!options.arguments.empty()) {
        status = apply_setting(span_of("device"), options.arguments[0U], settings_, error);
    }
    if (is_ok(status)) {
        Options connect{options};
        connect.connection = settings_.connection;
        exit_code_ = session_.run(connect);
    } else {
        usage_error(error);
    }
}

void Shell::show() noexcept {
    const ConnectionSettings& connection{settings_.connection};
    out_->print(Line{}.text("Device").pad(kLabelWidth).text(connection.device.text()));
    out_->print(Line{}
                    .text("Baud rate")
                    .pad(kLabelWidth)
                    .number(static_cast<std::uint32_t>(connection.baud_rate))
                    .text(" bit/s"));
    out_->print(Line{}
                    .text("Protocol")
                    .pad(kLabelWidth)
                    .character(connection.protocol)
                    .text(" (")
                    .text(std::string_view{protocol_name(connection.protocol)})
                    .character(')'));
    out_->print(
        Line{}.text("Timeout").pad(kLabelWidth).number(milliseconds(connection.timeouts.command)).text(" ms"));
    out_->print(Line{}.text("Interval").pad(kLabelWidth).number(milliseconds(settings_.interval)).text(" ms"));
    Line count{};
    count.text("Count").pad(kLabelWidth).number(settings_.count);
    if (settings_.count == 0U) {
        count.text(" (until Ctrl+C)");
    }
    out_->print(count);
    out_->print(Line{}.text("Verbose").pad(kLabelWidth).text(settings_.verbose ? "on" : "off"));
    out_->print(Line{}.text("Read-only").pad(kLabelWidth).text(settings_.read_only ? "on" : "off"));
    Line state{};
    state.text("Connection").pad(kLabelWidth);
    const Controller& controller{session_.controller()};
    if (controller.connected()) {
        state.text(controller.identity()).text(" on ").text(controller.settings().device.text());
    } else {
        state.text("not connected");
    }
    out_->print(state);
}

void Shell::usage_error(const Line& error) noexcept {
    err_->print(Line{}.text(kProgram).text(": ").text(error.view()));
    exit_code_ = kExitUsage;
}

}  // namespace

std::int32_t run_shell(const Options& options, const common::NativeHandle input, Console& out, Console& err,
                       const common::NativeHandle cancel_descriptor) noexcept {
    Shell shell{options, input, out, err, cancel_descriptor};
    return shell.run();
}

}  // namespace cli
}  // namespace elm327
