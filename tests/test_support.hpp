/// @file test_support.hpp
/// @brief Conversions between the library's spans and the standard types the tests compare with.

#ifndef ELM327_TEST_SUPPORT_HPP
#define ELM327_TEST_SUPPORT_HPP

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "common/containers/span.hpp"
#include "elm327/adapter/response.hpp"

namespace test {

/// An eventfd that becomes readable once signal() is called: a cancel descriptor for the tests.
class Event final {
public:
    Event() : descriptor_{::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)} {}
    ~Event() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    [[nodiscard]] int descriptor() const {
        return descriptor_;
    }

    void signal() const {
        const std::uint64_t one{1U};
        // A void cast does not silence warn_unused_result (glibc with _FORTIFY_SOURCE).
        const ssize_t written{::write(descriptor_, &one, sizeof(one))};
        static_cast<void>(written);
    }

private:
    int descriptor_;
};

inline common::Span<const char> span_of(const std::string_view text) {
    return common::Span<const char>{text.data(), text.size()};
}

inline common::Span<const std::uint8_t> bytes_of(const std::vector<std::uint8_t>& bytes) {
    return common::Span<const std::uint8_t>{bytes.data(), bytes.size()};
}

inline std::string string_of(const common::Span<const char> text) {
    return std::string{text.data(), text.size()};
}

inline std::vector<std::uint8_t> vector_of(const common::Span<const std::uint8_t> bytes) {
    return std::vector<std::uint8_t>{bytes.begin(), bytes.end()};
}

/// A response with the given lines, as the adapter would have printed them.
inline elm327::Response make_response(const std::initializer_list<std::string_view> lines) {
    elm327::Response response{};
    for (const std::string_view line : lines) {
        response.add_line(span_of(line), false);
    }
    return response;
}

}  // namespace test

#endif  // ELM327_TEST_SUPPORT_HPP
