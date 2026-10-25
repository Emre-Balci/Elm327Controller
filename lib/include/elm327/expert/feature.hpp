/// @file feature.hpp
/// @brief Features of a car that can be turned on and off, and the engine that does it safely.
///
/// Most "hidden features" of a car are one setting of one control module: a bit, or a small field,
/// in a value the module keeps under a data identifier (DID) -- its coding, or an adaptation. A
/// Feature describes one: the module (its CAN identifiers), how to recognise it (a DID whose value
/// starts with a known part number), the DID that holds the setting and its length, and which bits
/// mean on and which off. Definitions come as text (see parse_features()), so that someone who has
/// verified them for a car can share them, and the user of a window can then turn the feature on
/// and off without seeing a single code.
///
/// FeatureCoder applies a definition through a Controller, and refuses rather than guesses:
/// - before it changes anything, it checks the module's identification and the length of the
///   setting, so a definition made for one module never writes into another;
/// - it reads the setting first and keeps that value as the backup, changes only the feature's
///   bits, writes in the definition's diagnostic session, reads again to verify, and returns the
///   module to its default session;
/// - restore() writes a backup back, with the same checks;
/// - it changes nothing in read-only mode (the Controller refuses), it cannot unlock a module that
///   needs security access, and it writes only what a plain ELM327 sends in one CAN frame.
/// Every operation sets the adapter's addressing for the module (header, receive filter, flow
/// control) and resets it to the vehicle's OBD addressing afterwards.

#ifndef ELM327_EXPERT_FEATURE_HPP
#define ELM327_EXPERT_FEATURE_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/containers/static_vector.hpp"
#include "common/core/status.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/controller/controller.hpp"
#include "elm327/expert/uds.hpp"
#include "elm327/obd/message.hpp"

namespace elm327 {
namespace expert {

/// Most features one list holds.
inline constexpr std::size_t kMaxFeatures{64U};
/// Longest setting a feature reads.
inline constexpr std::size_t kMaxFeatureValue{64U};
/// Longest setting a feature writes: a WriteDataByIdentifier of a two-byte DID in one CAN frame
/// (2E, the DID and 4 bytes), the most a plain ELM327 sends.
inline constexpr std::size_t kMaxFeatureWrite{4U};
/// Most part numbers that identify the module of a feature.
inline constexpr std::size_t kMaxPartNumbers{4U};

using FeatureName = common::StaticString<48U>;
using FeatureText = common::StaticString<200U>;
/// A CAN identifier in hexadecimal: 3 digits (11 bit) or 8 (29 bit).
using CanId = common::StaticString<8U>;
using PartNumber = common::StaticString<24U>;

/// One feature: where it lives, how to recognise its module, and which bits turn it on and off.
struct Feature final {
    FeatureName name{};
    FeatureName group{};  ///< Usually the module, e.g. "Instrument cluster"; features are listed by it.
    FeatureText about{};  ///< What the feature does, for the user.
    FeatureText note{};   ///< When it takes effect, or what to watch out for.
    CanId request{};      ///< The CAN identifier the module listens to, e.g. "714".
    CanId reply{};        ///< The CAN identifier it answers from, e.g. "77E".
    /// ISO-TP extended addressing (as on BMW): the module's address goes in the first data byte.
    bool extended{false};
    std::uint8_t address{0U};
    std::uint8_t session{0x03U};      ///< The diagnostic session of the change; 03 is the extended one.
    std::uint16_t identify_did{0U};   ///< A DID whose value identifies the module, e.g. F187.
    /// The value of identify_did must start with one of these; empty means any module (written "*").
    common::StaticVector<PartNumber, kMaxPartNumbers> part_numbers{};
    std::uint16_t did{0U};     ///< The DID that holds the setting, e.g. 0600 (a coding).
    std::uint8_t length{0U};   ///< The length of its value in bytes.
    std::uint8_t byte{0U};     ///< The byte of the value that holds the feature, from 0.
    std::uint8_t mask{0U};     ///< The feature's bits in that byte.
    std::uint8_t on{0U};       ///< Those bits when the feature is on.
    std::uint8_t off{0U};      ///< Those bits when it is off.
};

using FeatureList = common::StaticVector<Feature, kMaxFeatures>;

/// Where and why parse_features() stopped.
struct FeatureParseError final {
    std::uint32_t line{0U};
    common::StaticString<120U> message{};
};

/// Parses feature definitions and appends them to @p features. The format:
/// @code
///   # A comment. Each [section] is one feature, named by its header.
///   [Needle sweep]
///   group    = Instrument cluster
///   about    = Sweeps the needles across the dials when the ignition comes on.
///   request  = 714
///   reply    = 77E
///   identify = F187 5G0920
///   did      = 0600
///   length   = 25
///   byte     = 1
///   bit      = 0
/// @endcode
/// - request, reply: the module's CAN identifiers, 3 or 8 hexadecimal digits;
/// - identify: a DID and the start of its value, one or more separated by "|", or "*" for any;
/// - did, length, byte: the DID of the setting, its length in bytes, and the byte with the feature;
/// - bit: one bit, 0 (the rightmost) to 7, set when on -- or mask, on and off in hexadecimal for a
///   field, or for a bit that is set when the feature is off;
/// - optional: group, about, note, session (hexadecimal, default 03), address (hexadecimal: the
///   module's extended address).
/// Lines that start with "#" or ";" are comments. Every definition needs the required keys, so
/// that a feature is never applied to a module it does not name.
/// @return kOk; kInvalidArgument with the line and the reason in @p error; kOutOfRange when there
///         would be more than kMaxFeatures features.
[[nodiscard]] common::Status parse_features(common::Span<const char> text, FeatureList& features,
                                            FeatureParseError& error) noexcept;

/// What a feature is on the connected vehicle.
enum class FeatureState : std::uint8_t {
    kOn,
    kOff,
    kOther,        ///< The feature's bits hold neither its on nor its off value.
    kNotThisEcu,   ///< A module answers, but it is not the one the definition is for.
    kNoAnswer,     ///< No module answers at the definition's address.
    kNeedsUnlock,  ///< The module refused the change without security access.
    kRefused,      ///< Read-only mode refused the change.
    kFailed,       ///< Anything else: see FeatureReading::reason.
};

/// @return a few words for @p state, e.g. "on", "not this module" or "needs security access".
[[nodiscard]] const char* to_string(FeatureState state) noexcept;

/// The state of @p feature in @p value, a value of its setting: kOn or kOff when the feature's
/// bits hold its on or off value, otherwise kOther; kFailed when @p value is shorter than the
/// definition's length.
[[nodiscard]] FeatureState feature_state(const Feature& feature, common::Span<const std::uint8_t> value) noexcept;

/// What FeatureCoder found or did.
struct FeatureReading final {
    FeatureState state{FeatureState::kFailed};
    /// Why, in words, when the state is not kOn, kOff or kOther, or when nothing had to change.
    common::StaticString<160U> reason{};
    /// The setting as it was read first: the backup that restore() writes back. Empty when it
    /// could not be read.
    common::StaticVector<std::uint8_t, kMaxFeatureValue> value{};
    /// What was written; empty when nothing was.
    common::StaticVector<std::uint8_t, kMaxFeatureValue> written{};
};

/// Turns features on and off through a Controller; see the file comment.
///
/// The object embeds a reply and its messages (about 9 KiB) and never allocates. Not thread-safe;
/// use it on the thread that uses the Controller.
class FeatureCoder final {
public:
    /// @p controller must be connected while the operations run, and outlive the coder.
    explicit FeatureCoder(Controller& controller) noexcept : controller_{&controller} {}

    /// Reads the state of @p feature. Changes nothing, so it works in read-only mode.
    [[nodiscard]] FeatureReading read(const Feature& feature) noexcept;

    /// Turns @p feature on or off. Nothing is written when it already is, when the module is not
    /// the one of the definition, or when the setting cannot be read first (there would be no
    /// backup).
    [[nodiscard]] FeatureReading change(const Feature& feature, bool on) noexcept;

    /// Writes @p backup, a value change() returned in FeatureReading::value, back to the setting
    /// of @p feature, after the same checks.
    [[nodiscard]] FeatureReading restore(const Feature& feature, common::Span<const std::uint8_t> backup) noexcept;

private:
    using Value = common::StaticVector<std::uint8_t, kMaxFeatureValue>;

    /// Sets the adapter's addressing for the module of @p feature. @return false when the adapter
    /// rejected it; @p reading then says why.
    bool address(const Feature& feature, FeatureReading& reading) noexcept;
    /// Resets the addressing to the vehicle's OBD addressing. Best effort.
    void reset_address(const Feature& feature) noexcept;
    /// Checks the identification of the module and reads the setting into reading.value.
    /// @return true with the state kOn, kOff or kOther; otherwise false, with the state and reason.
    bool identify_and_read(const Feature& feature, FeatureReading& reading) noexcept;
    /// Reads the setting into @p value. @return false with the state and reason in @p reading.
    bool read_setting(const Feature& feature, Value& value, FeatureReading& reading) noexcept;
    /// Writes @p value to the setting in the feature's session, verifies it, and returns to the
    /// default session. Sets the state and reading.written.
    void write_setting(const Feature& feature, common::Span<const std::uint8_t> value,
                       FeatureReading& reading) noexcept;
    /// Sends an AT command. @return kOk; kNotSupported when the adapter answers with an error.
    common::Status at(common::Span<const char> command) noexcept;
    /// Sends the AT command @p prefix followed by @p argument, e.g. "ATSH" and "714".
    /// @return true; false when it failed, with the state and reason in @p reading.
    bool send_at(std::string_view prefix, common::Span<const char> argument, FeatureReading& reading) noexcept;
    /// Sends @p command and decodes the reply into @p exchange.
    /// @return kOk with a positive or a negative reply; kPermissionDenied (read-only); the status
    ///         of the adapter's error message (kNotFound for "NO DATA"); otherwise the errors of
    ///         Controller::execute() and decode_uds_response().
    common::Status request(const UdsCommand& command, UdsExchange& exchange) noexcept;

    Controller* controller_;
    Response response_{};
    MessageList messages_{};
};

}  // namespace expert
}  // namespace elm327

#endif  // ELM327_EXPERT_FEATURE_HPP
