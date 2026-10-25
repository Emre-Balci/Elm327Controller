#include "elm327/expert/feature.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/protocol/text.hpp"
#include "elm327/controller/explain.hpp"

namespace elm327 {
namespace expert {
namespace {

constexpr std::uint8_t kDefaultSession{0x01U};
constexpr std::uint8_t kSecurityAccessDenied{0x33U};
constexpr std::size_t kCanId11{3U};
constexpr std::size_t kCanId29{8U};
constexpr std::uint32_t kBitsPerByte{8U};

/// The reasons of a FeatureReading.
using Reason = common::StaticString<160U>;

common::Span<const char> span_of(const std::string_view text) noexcept {
    return common::Span<const char>{text.data(), text.size()};
}

/// Appends as much of @p part to @p text as fits: a reason cut short beats none.
template <std::size_t Capacity>
void put(common::StaticString<Capacity>& text, const common::Span<const char> part) noexcept {
    const std::size_t room{common::StaticString<Capacity>::capacity() - text.size()};
    static_cast<void>(text.append((part.size() <= room) ? part : part.first(room)));
}

template <std::size_t Capacity>
void put(common::StaticString<Capacity>& text, const std::string_view part) noexcept {
    put(text, span_of(part));
}

/// Sets @p reading to @p state, with the reason @p text.
void set(FeatureReading& reading, const FeatureState state, const std::string_view text) noexcept {
    reading.state = state;
    reading.reason.clear();
    put(reading.reason, text);
}

/// Appends " <nrc name> (0x<nrc>)." to @p reason.
void put_nrc(Reason& reason, const std::uint8_t nrc) noexcept {
    put(reason, std::string_view{nrc_name(nrc)});
    put(reason, " (0x");
    static_cast<void>(common::append_hex(reason, nrc, 2U));
    put(reason, ").");
}

bool same(const common::Span<const std::uint8_t> first, const common::Span<const std::uint8_t> second) noexcept {
    bool equal{first.size() == second.size()};
    for (std::size_t index{0U}; equal && (index < first.size()); ++index) {
        equal = first[index] == second[index];
    }
    return equal;
}

// Parsing.

/// What the definition being parsed has said so far.
struct Seen final {
    std::uint32_t section_line{0U};
    bool request{false};
    bool reply{false};
    bool identify{false};
    bool did{false};
    bool length{false};
    bool byte{false};
    bool bit{false};
    bool mask{false};
    bool on{false};
    bool off{false};
};

/// A CAN identifier: 3 or 8 hexadecimal digits.
bool parse_can_id(const common::Span<const char> text, CanId& id) noexcept {
    const bool valid{((text.size() == kCanId11) || (text.size() == kCanId29)) &&
                     common::parse_hex<std::uint32_t>(text).has_value()};
    if (valid) {
        id.clear();
        static_cast<void>(id.append(text));  // Cannot fail: at most 8 characters.
    }
    return valid;
}

/// One or two hexadecimal digits.
bool parse_hex_byte(const common::Span<const char> text, std::uint8_t& value) noexcept {
    const common::Result<std::uint8_t> parsed{((!text.empty()) && (text.size() <= 2U))
                                                  ? common::parse_hex<std::uint8_t>(text)
                                                  : common::Result<std::uint8_t>::failure(
                                                        common::Status::kInvalidArgument)};
    value = parsed.value_or(value);
    return parsed.has_value();
}

/// A DID: one to four hexadecimal digits.
bool parse_did(const common::Span<const char> text, std::uint16_t& did) noexcept {
    const common::Result<std::uint16_t> parsed{((!text.empty()) && (text.size() <= 4U))
                                                   ? common::parse_hex<std::uint16_t>(text)
                                                   : common::Result<std::uint16_t>::failure(
                                                         common::Status::kInvalidArgument)};
    did = parsed.value_or(did);
    return parsed.has_value();
}

/// A decimal number from @p least to @p most.
bool parse_number(const common::Span<const char> text, const std::uint32_t least, const std::uint32_t most,
                  std::uint8_t& value) noexcept {
    const common::Result<std::uint32_t> parsed{common::parse_unsigned<std::uint32_t>(text)};
    const bool valid{parsed.has_value() && (parsed.value() >= least) && (parsed.value() <= most)};
    if (valid) {
        value = static_cast<std::uint8_t>(parsed.value());
    }
    return valid;
}

template <std::size_t Capacity>
bool parse_text(const common::Span<const char> text, common::StaticString<Capacity>& out) noexcept {
    out.clear();
    return out.append(text);  // false when it is too long.
}

/// "<DID> <part number>|<part number>...", or "<DID> *" for any module.
bool parse_identify(const common::Span<const char> text, Feature& feature) noexcept {
    std::size_t space{0U};
    while ((space < text.size()) && (!common::is_space(text[space]))) {
        ++space;
    }
    const common::Span<const char> did{text.first(space)};
    const common::Span<const char> parts{common::trim(text.subspan(space))};
    bool valid{parse_did(did, feature.identify_did) && (!parts.empty())};
    feature.part_numbers.clear();
    if (valid && (!common::equals(parts, "*"))) {
        std::size_t start{0U};
        for (std::size_t index{0U}; valid && (index <= parts.size()); ++index) {
            if ((index == parts.size()) || (parts[index] == '|')) {
                const common::Span<const char> part{common::trim(parts.subspan(start, index - start))};
                PartNumber number{};
                valid = (!part.empty()) && number.append(part) && feature.part_numbers.push_back(number);
                start = index + 1U;
            }
        }
    }
    return valid;
}

class Parser final {
public:
    Parser(FeatureList& features, FeatureParseError& error) noexcept : features_{&features}, error_{&error} {}

    common::Status line(std::uint32_t number, common::Span<const char> text) noexcept;
    /// Checks the last definition.
    common::Status finish() noexcept;

private:
    common::Status section(common::Span<const char> header) noexcept;
    common::Status key_value(common::Span<const char> key, common::Span<const char> value) noexcept;
    /// Checks that the current definition is complete and consistent.
    common::Status complete() noexcept;
    /// Sets one of the flags of seen_, failing when the key was given before.
    common::Status once(bool& flag, common::Span<const char> key) noexcept;
    common::Status fail(std::uint32_t line, std::string_view reason, common::Span<const char> subject) noexcept;

    FeatureList* features_;
    FeatureParseError* error_;
    Feature* current_{nullptr};
    Seen seen_{};
    std::uint32_t line_{0U};
};

common::Status Parser::line(const std::uint32_t number, const common::Span<const char> text) noexcept {
    line_ = number;
    const common::Span<const char> trimmed{common::trim(text)};
    common::Status status{common::Status::kOk};
    if (trimmed.empty() || (trimmed[0U] == '#') || (trimmed[0U] == ';')) {
        // A blank line or a comment.
    } else if (trimmed[0U] == '[') {
        status = section(trimmed);
    } else if (current_ == nullptr) {
        status = fail(line_, "a key before the first [feature]", trimmed);
    } else {
        const common::Result<std::size_t> equals_sign{common::find(trimmed, "=")};
        if (!equals_sign.has_value()) {
            status = fail(line_, "expected 'key = value'", trimmed);
        } else {
            status = key_value(common::trim(trimmed.first(equals_sign.value())),
                               common::trim(trimmed.subspan(equals_sign.value() + 1U)));
        }
    }
    return status;
}

common::Status Parser::finish() noexcept {
    return (current_ != nullptr) ? complete() : common::Status::kOk;
}

common::Status Parser::section(const common::Span<const char> header) noexcept {
    common::Status status{(current_ != nullptr) ? complete() : common::Status::kOk};
    const bool closed{header[header.size() - 1U] == ']'};
    const common::Span<const char> name{closed ? common::trim(header.subspan(1U, header.size() - 2U))
                                               : common::Span<const char>{}};
    if (!is_ok(status)) {
        // The definition before is incomplete: reported.
    } else if (!closed) {
        status = fail(line_, "a feature's header ends with ']'", header);
    } else if (name.empty()) {
        status = fail(line_, "a feature needs a name", header);
    } else if (!features_->emplace_back()) {
        error_->line = line_;
        error_->message.clear();
        put(error_->message, "too many features (at most 64)");
        status = common::Status::kOutOfRange;
    } else {
        current_ = &features_->back();
        seen_ = Seen{};
        seen_.section_line = line_;
        if (!current_->name.append(name)) {
            status = fail(line_, "the name is too long", name);
        }
    }
    return status;
}

common::Status Parser::key_value(const common::Span<const char> key, const common::Span<const char> value) noexcept {
    Feature& feature{*current_};
    common::Status status{common::Status::kOk};
    bool valid{true};
    if (common::equals(key, "group")) {
        valid = parse_text(value, feature.group);
    } else if (common::equals(key, "about")) {
        valid = parse_text(value, feature.about);
    } else if (common::equals(key, "note")) {
        valid = parse_text(value, feature.note);
    } else if (common::equals(key, "request")) {
        status = once(seen_.request, key);
        valid = parse_can_id(value, feature.request);
    } else if (common::equals(key, "reply")) {
        status = once(seen_.reply, key);
        valid = parse_can_id(value, feature.reply);
    } else if (common::equals(key, "address")) {
        valid = parse_hex_byte(value, feature.address);
        feature.extended = true;
    } else if (common::equals(key, "session")) {
        valid = parse_hex_byte(value, feature.session);
    } else if (common::equals(key, "identify")) {
        status = once(seen_.identify, key);
        valid = parse_identify(value, feature);
    } else if (common::equals(key, "did")) {
        status = once(seen_.did, key);
        valid = parse_did(value, feature.did);
    } else if (common::equals(key, "length")) {
        status = once(seen_.length, key);
        valid = parse_number(value, 1U, kMaxFeatureValue, feature.length);
    } else if (common::equals(key, "byte")) {
        status = once(seen_.byte, key);
        valid = parse_number(value, 0U, kMaxFeatureValue - 1U, feature.byte);
    } else if (common::equals(key, "bit")) {
        status = once(seen_.bit, key);
        std::uint8_t bit{0U};
        valid = parse_number(value, 0U, kBitsPerByte - 1U, bit);
        feature.mask = static_cast<std::uint8_t>(1U << bit);
        feature.on = feature.mask;
        feature.off = 0U;
    } else if (common::equals(key, "mask")) {
        status = once(seen_.mask, key);
        valid = parse_hex_byte(value, feature.mask);
    } else if (common::equals(key, "on")) {
        status = once(seen_.on, key);
        valid = parse_hex_byte(value, feature.on);
    } else if (common::equals(key, "off")) {
        status = once(seen_.off, key);
        valid = parse_hex_byte(value, feature.off);
    } else {
        status = fail(line_, "unknown key", key);
    }
    if (is_ok(status) && (!valid)) {
        error_->line = line_;
        error_->message.clear();
        put(error_->message, "invalid value for '");
        put(error_->message, key);
        put(error_->message, "'");
        if (!value.empty()) {
            put(error_->message, ": '");
            put(error_->message, value);
            put(error_->message, "'");
        }
        status = common::Status::kInvalidArgument;
    }
    return status;
}

common::Status Parser::once(bool& flag, const common::Span<const char> key) noexcept {
    const bool repeated{flag};
    flag = true;
    return repeated ? fail(line_, "given twice", key) : common::Status::kOk;
}

common::Status Parser::complete() noexcept {
    const Feature& feature{*current_};
    const std::uint32_t line{seen_.section_line};
    const common::Span<const char> name{feature.name.text()};
    const bool field{seen_.mask || seen_.on || seen_.off};
    const std::uint32_t outside{(static_cast<std::uint32_t>(feature.on) | static_cast<std::uint32_t>(feature.off)) &
                                (~static_cast<std::uint32_t>(feature.mask))};
    common::Status status{common::Status::kOk};
    if (!seen_.request) {
        status = fail(line, "missing 'request', the module's CAN identifier, in", name);
    } else if (!seen_.reply) {
        status = fail(line, "missing 'reply', the CAN identifier of the module's replies, in", name);
    } else if (!seen_.identify) {
        status = fail(line, "missing 'identify', how to recognise the module, in", name);
    } else if (!seen_.did) {
        status = fail(line, "missing 'did', the identifier of the setting, in", name);
    } else if (!seen_.length) {
        status = fail(line, "missing 'length', the setting's length in bytes, in", name);
    } else if (!seen_.byte) {
        status = fail(line, "missing 'byte', the byte with the feature, in", name);
    } else if (seen_.bit && field) {
        status = fail(line, "use 'bit', or 'mask', 'on' and 'off', not both, in", name);
    } else if ((!seen_.bit) && (!(seen_.mask && seen_.on && seen_.off))) {
        status = fail(line, "missing 'bit' (or 'mask', 'on' and 'off') in", name);
    } else if (feature.byte >= feature.length) {
        status = fail(line, "'byte' must be less than 'length' in", name);
    } else if (feature.mask == 0U) {
        status = fail(line, "'mask' has no bits in", name);
    } else if (outside != 0U) {
        status = fail(line, "'on' and 'off' must stay within 'mask' in", name);
    } else if (feature.on == feature.off) {
        status = fail(line, "'on' and 'off' must differ in", name);
    } else {
        // Complete.
    }
    return status;
}

common::Status Parser::fail(const std::uint32_t line, const std::string_view reason,
                            const common::Span<const char> subject) noexcept {
    error_->line = line;
    error_->message.clear();
    put(error_->message, reason);
    if (!subject.empty()) {
        put(error_->message, " '");
        put(error_->message, subject);
        put(error_->message, "'");
    }
    return common::Status::kInvalidArgument;
}

}  // namespace

common::Status parse_features(const common::Span<const char> text, FeatureList& features,
                              FeatureParseError& error) noexcept {
    error = FeatureParseError{};
    const std::size_t before{features.size()};
    Parser parser{features, error};
    common::Status status{common::Status::kOk};
    std::size_t start{0U};
    std::uint32_t number{0U};
    for (std::size_t index{0U}; is_ok(status) && (index <= text.size()); ++index) {
        if ((index == text.size()) || (text[index] == '\n')) {
            ++number;
            status = parser.line(number, text.subspan(start, index - start));
            start = index + 1U;
        }
    }
    if (is_ok(status)) {
        status = parser.finish();
    }
    // On an error, nothing of this text is kept: a list never holds a half-checked definition.
    for (std::size_t count{features.size()}; (!is_ok(status)) && (count > before); --count) {
        features.pop_back();
    }
    return status;
}

const char* to_string(const FeatureState state) noexcept {
    const char* text{"failed"};
    switch (state) {
        case FeatureState::kOn:
            text = "on";
            break;
        case FeatureState::kOff:
            text = "off";
            break;
        case FeatureState::kOther:
            text = "another value";
            break;
        case FeatureState::kNotThisEcu:
            text = "not this module";
            break;
        case FeatureState::kNoAnswer:
            text = "no answer";
            break;
        case FeatureState::kNeedsUnlock:
            text = "needs security access";
            break;
        case FeatureState::kRefused:
            text = "refused (read-only)";
            break;
        case FeatureState::kFailed:
            text = "failed";
            break;
    }
    return text;
}

FeatureState feature_state(const Feature& feature, const common::Span<const std::uint8_t> value) noexcept {
    FeatureState state{FeatureState::kFailed};
    if ((value.size() >= feature.length) && (feature.byte < value.size())) {
        const std::uint32_t bits{static_cast<std::uint32_t>(value[feature.byte]) &
                                 static_cast<std::uint32_t>(feature.mask)};
        if (bits == static_cast<std::uint32_t>(feature.on)) {
            state = FeatureState::kOn;
        } else if (bits == static_cast<std::uint32_t>(feature.off)) {
            state = FeatureState::kOff;
        } else {
            state = FeatureState::kOther;  // Someone set another value, or the definition does not fit.
        }
    }
    return state;
}

FeatureReading FeatureCoder::read(const Feature& feature) noexcept {
    FeatureReading reading{};
    if (address(feature, reading)) {
        static_cast<void>(identify_and_read(feature, reading));
    }
    reset_address(feature);
    return reading;
}

FeatureReading FeatureCoder::change(const Feature& feature, const bool on) noexcept {
    FeatureReading reading{};
    bool known{address(feature, reading)};
    if (known) {
        known = identify_and_read(feature, reading);
    }
    if (known) {
        const FeatureState wanted{on ? FeatureState::kOn : FeatureState::kOff};
        if (reading.state == wanted) {
            put(reading.reason, on ? "It is on already." : "It is off already.");
        } else {
            Value value{reading.value};
            const std::uint32_t kept{static_cast<std::uint32_t>(value[feature.byte]) &
                                     (~static_cast<std::uint32_t>(feature.mask))};
            const std::uint32_t bits{on ? static_cast<std::uint32_t>(feature.on)
                                        : static_cast<std::uint32_t>(feature.off)};
            value[feature.byte] = static_cast<std::uint8_t>((kept | bits) & 0xFFU);
            write_setting(feature, common::Span<const std::uint8_t>{value.data(), value.size()}, reading);
        }
    }
    reset_address(feature);
    return reading;
}

FeatureReading FeatureCoder::restore(const Feature& feature, const common::Span<const std::uint8_t> backup) noexcept {
    FeatureReading reading{};
    if (backup.size() != feature.length) {
        set(reading, FeatureState::kFailed, "The backup does not fit this setting.");
    } else {
        bool known{address(feature, reading)};
        if (known) {
            known = identify_and_read(feature, reading);
        }
        if (known && same(common::Span<const std::uint8_t>{reading.value.data(), reading.value.size()}, backup)) {
            put(reading.reason, "It holds that value already.");
        } else if (known) {
            write_setting(feature, backup, reading);
        } else {
            // Not the module of the definition, or it does not answer: nothing is written.
        }
        reset_address(feature);
    }
    return reading;
}

bool FeatureCoder::address(const Feature& feature, FeatureReading& reading) noexcept {
    const common::Span<const char> request{feature.request.text()};
    const bool long_id{request.size() == kCanId29};
    // 29-bit identifiers: the priority byte (ATCP), then the other three (ATSH).
    bool ok{long_id ? send_at("ATCP", request.first(2U), reading) : send_at("ATSH", request, reading)};
    if (ok && long_id) {
        ok = send_at("ATSH", request.subspan(2U), reading);
    }
    if (ok) {
        ok = send_at("ATCRA", feature.reply.text(), reading);
    }
    // Flow control to the module, so that it sends long replies to us.
    if (ok) {
        ok = send_at("ATFCSH", request, reading);
    }
    if (ok) {
        ok = send_at("ATFCSD", span_of("300000"), reading);
    }
    if (ok) {
        ok = send_at("ATFCSM", span_of("1"), reading);
    }
    if (ok && feature.extended) {
        std::array<char, 2U> address{};
        static_cast<void>(common::format_hex(feature.address, 2U, address));
        ok = send_at("ATCEA", common::Span<const char>{address.data(), address.size()}, reading);
    }
    return ok;
}

void FeatureCoder::reset_address(const Feature& feature) noexcept {
    // Back to the vehicle's OBD addressing: the functional header of its bus, any reply.
    static_cast<void>(at(span_of("ATFCSM0")));
    static_cast<void>(at(span_of("ATCRA")));
    if (feature.extended) {
        static_cast<void>(at(span_of("ATCEA")));
    }
    if (feature.request.size() == kCanId29) {
        static_cast<void>(at(span_of("ATCP18")));
        static_cast<void>(at(span_of("ATSHDB33F1")));
    } else {
        static_cast<void>(at(span_of("ATSH7DF")));
    }
}

bool FeatureCoder::identify_and_read(const Feature& feature, FeatureReading& reading) noexcept {
    UdsCommand command{};
    UdsExchange exchange{};
    static_cast<void>(uds_read_data_by_identifier(feature.identify_did, command));  // Cannot fail: fixed size.
    const common::Status status{request(command, exchange)};
    const common::Span<const std::uint8_t> data{exchange.data.data(), exchange.data.size()};
    bool ok{false};
    if (status == common::Status::kNotFound) {
        set(reading, FeatureState::kNoAnswer, "No module answers at ");
        put(reading.reason, feature.request.text());
        put(reading.reason, ": is it fitted, and the ignition on?");
    } else if (!is_ok(status)) {
        set(reading, FeatureState::kFailed, "Reading the module's identification failed: ");
        put(reading.reason, explain_failure(status, response_).reason);
    } else if ((!exchange.positive) || (data.size() < 2U)) {
        set(reading, FeatureState::kNotThisEcu, "The module does not report its identification.");
    } else {
        // The identification, after the two bytes of the DID, as text.
        std::array<char, kMaxReplyData> text{};
        const common::Result<std::size_t> length{format_printable(data.subspan(2U), text)};
        const common::Span<const char> found{text.data(), length.value_or(0U)};
        bool matches{feature.part_numbers.empty()};
        for (const PartNumber& number : feature.part_numbers) {
            matches = matches || common::starts_with(found, number.view());
        }
        if (matches) {
            Value value{};
            ok = read_setting(feature, value, reading);
            if (ok) {
                reading.value = value;
                reading.state = feature_state(feature, common::Span<const std::uint8_t>{value.data(), value.size()});
            }
        } else {
            set(reading, FeatureState::kNotThisEcu, "This module is '");
            put(reading.reason, found);
            put(reading.reason, "'; the definition is for '");
            put(reading.reason, feature.part_numbers[0U].text());
            put(reading.reason, (feature.part_numbers.size() > 1U) ? "' and others." : "'.");
        }
    }
    return ok;
}

bool FeatureCoder::read_setting(const Feature& feature, Value& value, FeatureReading& reading) noexcept {
    UdsCommand command{};
    UdsExchange exchange{};
    static_cast<void>(uds_read_data_by_identifier(feature.did, command));  // Cannot fail: fixed size.
    const common::Status status{request(command, exchange)};
    const common::Span<const std::uint8_t> data{exchange.data.data(), exchange.data.size()};
    const bool echoed{(data.size() >= 2U) && (data[0U] == static_cast<std::uint8_t>(feature.did >> kBitsPerByte)) &&
                      (data[1U] == static_cast<std::uint8_t>(feature.did & 0xFFU))};
    bool ok{false};
    if (!is_ok(status)) {
        set(reading, FeatureState::kFailed, "Reading the setting failed: ");
        put(reading.reason, explain_failure(status, response_).reason);
    } else if (!exchange.positive) {
        set(reading, FeatureState::kFailed, "The module refused to read the setting: ");
        put_nrc(reading.reason, exchange.nrc);
    } else if ((!echoed) || exchange.truncated) {
        set(reading, FeatureState::kFailed, "The module's reply to reading the setting is not as expected.");
    } else if ((data.size() - 2U) != feature.length) {
        set(reading, FeatureState::kNotThisEcu, "The setting has ");
        static_cast<void>(common::append_unsigned(reading.reason, data.size() - 2U));
        put(reading.reason, " bytes, the definition expects ");
        static_cast<void>(common::append_unsigned(reading.reason, std::uint32_t{feature.length}));
        put(reading.reason, ": it is for another module.");
    } else {
        value.clear();
        for (std::size_t index{2U}; index < data.size(); ++index) {
            static_cast<void>(value.push_back(data[index]));  // Cannot fail: length is at most the capacity.
        }
        ok = true;
    }
    return ok;
}

void FeatureCoder::write_setting(const Feature& feature, const common::Span<const std::uint8_t> value,
                                 FeatureReading& reading) noexcept {
    UdsCommand command{};
    UdsExchange exchange{};
    bool proceed{value.size() <= kMaxFeatureWrite};
    if (!proceed) {
        set(reading, FeatureState::kFailed, "The setting has ");
        static_cast<void>(common::append_unsigned(reading.reason, value.size()));
        put(reading.reason, " bytes, but a plain ELM327 sends at most 4 in one CAN frame, so it cannot be written.");
    }
    // The session of the change; the default one needs no request.
    const bool own_session{proceed && (feature.session != kDefaultSession)};
    bool in_session{false};
    if (own_session) {
        const std::array<std::uint8_t, 1U> session{{feature.session}};
        static_cast<void>(uds_request(service_id(UdsService::kDiagnosticSessionControl), session, command));
        const common::Status status{request(command, exchange)};
        if (status == common::Status::kPermissionDenied) {
            set(reading, FeatureState::kRefused, "Read-only mode is on: nothing was changed.");
        } else if (!is_ok(status)) {
            set(reading, FeatureState::kFailed, "Entering the diagnostic session failed: ");
            put(reading.reason, explain_failure(status, response_).reason);
        } else if (!exchange.positive) {
            set(reading, FeatureState::kFailed, "The module refused the diagnostic session: ");
            put_nrc(reading.reason, exchange.nrc);
        } else {
            in_session = true;
        }
        proceed = in_session;
    }
    bool written{false};
    if (proceed) {
        static_cast<void>(uds_write_data_by_identifier(feature.did, value, command));  // Fits: checked above.
        const common::Status status{request(command, exchange)};
        if (status == common::Status::kPermissionDenied) {
            set(reading, FeatureState::kRefused, "Read-only mode is on: nothing was changed.");
        } else if (!is_ok(status)) {
            set(reading, FeatureState::kFailed, "Writing the setting failed: ");
            put(reading.reason, explain_failure(status, response_).reason);
        } else if ((!exchange.positive) && (exchange.nrc == kSecurityAccessDenied)) {
            set(reading, FeatureState::kNeedsUnlock,
                "The module needs security access for this change, which this tool cannot do. Nothing was changed.");
        } else if (!exchange.positive) {
            set(reading, FeatureState::kFailed, "The module refused the change: ");
            put_nrc(reading.reason, exchange.nrc);
        } else {
            written = true;
            reading.written.clear();
            for (const std::uint8_t byte : value) {
                static_cast<void>(reading.written.push_back(byte));  // Cannot fail: at most kMaxFeatureWrite.
            }
        }
    }
    // Verify: read the setting again.
    if (written) {
        Value check{};
        if (read_setting(feature, check, reading)) {
            const common::Span<const std::uint8_t> now{check.data(), check.size()};
            if (same(now, value)) {
                reading.state = feature_state(feature, now);
                reading.reason.clear();
            } else {
                set(reading, FeatureState::kFailed, "The module holds another value than the one written.");
            }
        }
    }
    // Back to the default session, whatever happened in the other one.
    if (in_session) {
        const std::array<std::uint8_t, 1U> session{{kDefaultSession}};
        static_cast<void>(uds_request(service_id(UdsService::kDiagnosticSessionControl), session, command));
        static_cast<void>(request(command, exchange));
    }
}

common::Status FeatureCoder::at(const common::Span<const char> command) noexcept {
    const common::Status status{controller_->execute(std::string_view{command.data(), command.size()}, response_)};
    return (is_ok(status) && is_error(response_.reply())) ? common::Status::kNotSupported : status;
}

bool FeatureCoder::send_at(const std::string_view prefix, const common::Span<const char> argument,
                           FeatureReading& reading) noexcept {
    common::StaticString<Adapter::kMaxCommandLength> command{};
    const bool built{command.append(prefix) && command.append(argument)};
    const common::Status status{built ? at(command.text()) : common::Status::kInvalidArgument};
    if (!is_ok(status)) {
        set(reading, FeatureState::kFailed, "The adapter does not take the module's address (");
        put(reading.reason, command.text());
        put(reading.reason, "): ");
        put(reading.reason, explain_failure(status, response_).reason);
    }
    return is_ok(status);
}

common::Status FeatureCoder::request(const UdsCommand& command, UdsExchange& exchange) noexcept {
    common::Status status{controller_->execute(command.view(), response_)};
    if (is_ok(status)) {
        status = is_error(response_.reply()) ? status_of(response_.reply())
                                             : decode_uds_response(response_, messages_, exchange);
    }
    return status;
}

}  // namespace expert
}  // namespace elm327
