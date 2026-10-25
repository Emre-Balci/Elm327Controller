/// @file explain.hpp
/// @brief Texts that tell a person why an operation failed.
///
/// A Status says what went wrong in terms of the program; a person wants the adapter's own words
/// ("NO DATA", "UNABLE TO CONNECT") and, where there is one, advice. The front-ends put these texts
/// into their messages and add advice of their own, e.g. the command-line option to use.

#ifndef ELM327_CONTROLLER_EXPLAIN_HPP
#define ELM327_CONTROLLER_EXPLAIN_HPP

#include <string_view>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "elm327/adapter/response.hpp"
#include "elm327/controller/controller.hpp"

namespace elm327 {

/// Why an operation failed.
struct Explanation final {
    /// The reason, e.g. "NO DATA", "interrupted" or "no such device". Views a static text or a line
    /// of the Response that was explained.
    common::Span<const char> reason{};
    /// Advice, e.g. "is the ignition on?"; empty when there is none.
    std::string_view advice{};
};

/// Explains why an operation of a Controller failed with @p status after the adapter replied
/// @p response (see Controller::last_response()): "interrupted" for kInterrupted; the adapter's
/// error message when it sent one, e.g. "NO DATA" or "UNABLE TO CONNECT" (with advice); "unexpected
/// reply" for kInvalidArgument (a reply that is not OBD data, or malformed); otherwise the
/// description of @p status, e.g. "timed out".
[[nodiscard]] Explanation explain_failure(common::Status status, const Response& response) noexcept;

/// Explains why opening the device failed with @p status (ConnectFailure::kOpen), e.g. "no such
/// device", or "permission denied" with the advice to join the dialout group (Linux).
[[nodiscard]] Explanation explain_open_failure(common::Status status) noexcept;

/// Explains why Controller::connect() failed with @p status at the step @p failure (see
/// Controller::connect_failure()): explain_open_failure() for ConnectFailure::kOpen; "the adapter
/// does not answer", with the advice to check the device and the baud rate, for
/// ConnectFailure::kNoAnswer; "the adapter rejected its configuration" for a kNotSupported while
/// initializing; otherwise "interrupted" or the description of @p status.
[[nodiscard]] Explanation explain_connect_failure(ConnectFailure failure, common::Status status) noexcept;

}  // namespace elm327

#endif  // ELM327_CONTROLLER_EXPLAIN_HPP
