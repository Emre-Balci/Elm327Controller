#include "test_support.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace test {
namespace {

void close_native(common::NativeHandle& handle) {
    if (handle != common::kInvalidNativeHandle) {
#if defined(_WIN32)
        static_cast<void>(::CloseHandle(handle));
#else
        static_cast<void>(::close(handle));
#endif
        handle = common::kInvalidNativeHandle;
    }
}

}  // namespace

Capture::Capture() {
#if defined(_WIN32)
    std::array<wchar_t, MAX_PATH + 1U> directory{};
    std::array<wchar_t, MAX_PATH + 1U> name{};
    const DWORD length{::GetTempPathW(static_cast<DWORD>(directory.size()), directory.data())};
    if ((length > 0U) && (length < directory.size()) &&
        (::GetTempFileNameW(directory.data(), L"elm", 0U, name.data()) != 0U)) {
        const HANDLE file{::CreateFileW(name.data(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr)};
        descriptor_ = (file != INVALID_HANDLE_VALUE) ? file : common::kInvalidNativeHandle;
    }
#elif defined(__ANDROID__)
    // Bionic declares memfd_create() from API level 30 on; the system call is older.
    descriptor_ = static_cast<common::NativeHandle>(::syscall(__NR_memfd_create, "capture", MFD_CLOEXEC));
#elif COMMON_LINUX_API
    descriptor_ = ::memfd_create("capture", MFD_CLOEXEC);
#else
    // No memfd_create() (macOS): a temporary file, removed at once, so that it goes with the descriptor.
    std::string name{"/tmp/elm327-capture-XXXXXX"};
    descriptor_ = ::mkstemp(name.data());
    if (descriptor_ >= 0) {
        static_cast<void>(::unlink(name.c_str()));
    }
#endif
}

Capture::~Capture() {
    close_native(descriptor_);
}

std::string Capture::text() const {
    std::string text{};
    std::array<char, 4096U> buffer{};
#if defined(_WIN32)
    static_cast<void>(::SetFilePointer(descriptor_, 0, nullptr, FILE_BEGIN));
    DWORD count{0U};
    while ((::ReadFile(descriptor_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) != FALSE) &&
           (count > 0U)) {
        text.append(buffer.data(), count);
    }
#else
    static_cast<void>(::lseek(descriptor_, 0, SEEK_SET));
    for (ssize_t count{::read(descriptor_, buffer.data(), buffer.size())}; count > 0;
         count = ::read(descriptor_, buffer.data(), buffer.size())) {
        text.append(buffer.data(), static_cast<std::size_t>(count));
    }
#endif
    return text;
}

Pipe::Pipe() {
#if defined(_WIN32)
    // Room for any script of the tests, which is written before anybody reads.
    constexpr DWORD kBufferSize{64U * 1024U};
    if (::CreatePipe(&read_, &write_, nullptr, kBufferSize) == FALSE) {
        read_ = common::kInvalidNativeHandle;
        write_ = common::kInvalidNativeHandle;
    }
#else
    std::array<int, 2U> ends{{-1, -1}};
#if COMMON_LINUX_API
    const bool created{::pipe2(ends.data(), O_CLOEXEC) == 0};
#else
    const bool created{(::pipe(ends.data()) == 0) && (::fcntl(ends[0U], F_SETFD, FD_CLOEXEC) == 0) &&
                       (::fcntl(ends[1U], F_SETFD, FD_CLOEXEC) == 0)};  // No pipe2() on macOS.
#endif
    if (created) {
        read_ = ends[0U];
        write_ = ends[1U];
    }
#endif
}

Pipe::~Pipe() {
    close_native(read_);
    close_native(write_);
}

void Pipe::write_and_close(const std::string& text) {
#if defined(_WIN32)
    DWORD written{0U};
    static_cast<void>(::WriteFile(write_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
#else
    const ssize_t written{::write(write_, text.data(), text.size())};
    static_cast<void>(written);
#endif
    close_native(write_);
}

}  // namespace test
