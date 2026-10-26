#include "gui/android_bridge.hpp"

#include <android/native_activity.h>
#include <jni.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/time/clock.hpp"
#include "sim/demo.hpp"

namespace {

/// What bluetoothState() of MainActivity answers, besides 1 for no Bluetooth; in step with
/// MainActivity.java.
constexpr jint kStateReady{0};
constexpr jint kStateOff{2};
constexpr jint kStateNoPermission{3};

/// The status byte that MainActivity answers on a link; in step with BluetoothLink.java.
constexpr std::uint8_t kLinkConnected{0U};
constexpr std::uint8_t kLinkNotFound{1U};
constexpr std::uint8_t kLinkNoPermission{2U};
constexpr std::uint8_t kLinkOff{3U};

/// Waits of open_link() for the status byte: one, unless a signal handler interrupts it.
constexpr std::uint32_t kMaxLinkWaits{100U};

/// The activity and the methods of MainActivity, found by begin().
struct Java final {
    ANativeActivity* activity{nullptr};
    JNIEnv* env{nullptr};  ///< That of the thread of android_main().
    jmethodID bluetooth_state{nullptr};
    jmethodID paired_devices{nullptr};
    jmethodID connect_bluetooth{nullptr};
    jmethodID show_keyboard{nullptr};
    jmethodID next_character{nullptr};
};

Java g_java{};

/// The insets in pixels, written on Java's main thread by nativeSetInsets() and read by the thread
/// of android_main().
std::atomic<std::int32_t> g_inset_left{0};
std::atomic<std::int32_t> g_inset_top{0};
std::atomic<std::int32_t> g_inset_right{0};
std::atomic<std::int32_t> g_inset_bottom{0};

/// Clears an exception that a call into Java left pending. @return true if there was one.
bool cleared_exception(JNIEnv* const env) noexcept {
    const bool thrown{env->ExceptionCheck() == JNI_TRUE};
    if (thrown) {
        env->ExceptionClear();
    }
    return thrown;
}

/// Copies the Java string @p text into @p out. @return false if it does not fit (@p out is empty
/// then) or there is none.
template <std::size_t Capacity>
bool copy_string(JNIEnv* const env, const jstring text, common::StaticString<Capacity>& out) noexcept {
    out.clear();
    bool copied{false};
    if (text != nullptr) {
        const char* const characters{env->GetStringUTFChars(text, nullptr)};
        if (characters != nullptr) {
            copied = out.append(std::string_view{characters});
            env->ReleaseStringUTFChars(text, characters);
        }
        if (!copied) {
            out.clear();
        }
    }
    return copied;
}

timespec to_relative_timespec(const common::Duration duration) noexcept {
    const std::int64_t ns{duration.count()};
    timespec ts{};
    ts.tv_sec = ns / common::MonotonicClock::kNanosPerSecond;
    ts.tv_nsec = ns % common::MonotonicClock::kNanosPerSecond;
    return ts;
}

/// Waits for the status byte of MainActivity on @p link, at most until @p deadline or until
/// @p cancel is readable. @return kOk once connected; otherwise why not, see open_link().
common::Status wait_for_link(const int link, const common::NativeHandle cancel,
                             const common::Deadline& deadline) noexcept {
    common::Status status{common::Status::kTimeout};
    bool waiting{true};
    for (std::uint32_t wait{0U}; waiting && (wait < kMaxLinkWaits); ++wait) {
        std::array<pollfd, 2U> entries{};
        entries[0U].fd = link;
        entries[0U].events = POLLIN;
        entries[1U].fd = cancel;  // poll() skips a negative descriptor.
        entries[1U].events = POLLIN;
        timespec timeout{};
        const timespec* timeout_pointer{nullptr};
        if (!deadline.is_never()) {
            timeout = to_relative_timespec(deadline.remaining());
            timeout_pointer = &timeout;
        }
        const int ready{::ppoll(entries.data(), entries.size(), timeout_pointer, nullptr)};
        waiting = (ready < 0) && (errno == EINTR);
        if (waiting) {
            // A signal handler ran: wait again.
        } else if (ready < 0) {
            status = common::Status::kSystemError;
        } else if (ready == 0) {
            status = common::Status::kTimeout;
        } else if (static_cast<std::uint32_t>(entries[1U].revents) != 0U) {
            status = common::Status::kInterrupted;
        } else {
            std::uint8_t answer{kLinkConnected};
            const ssize_t count{::read(link, &answer, 1U)};
            if (count != 1) {
                status = common::Status::kDisconnected;  // MainActivity closed the link.
            } else if (answer == kLinkConnected) {
                status = common::Status::kOk;
            } else if (answer == kLinkNotFound) {
                status = common::Status::kNotFound;
            } else if (answer == kLinkNoPermission) {
                status = common::Status::kPermissionDenied;
            } else if (answer == kLinkOff) {
                status = common::Status::kNotRunning;
            } else {
                status = common::Status::kDisconnected;  // The adapter refused the connection.
            }
        }
    }
    return status;
}

/// Asks MainActivity to connect to the Bluetooth adapter at @p address and to link it to @p link,
/// one end of a socket pair, which MainActivity owns from then on. Runs on the worker's thread,
/// which is attached to Java for the call.
void connect_bluetooth(const char* const address, const int link) noexcept {
    JavaVM* const vm{g_java.activity->vm};
    JNIEnv* env{nullptr};
    if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        const jstring text{env->NewStringUTF(address)};
        if (text != nullptr) {
            env->CallVoidMethod(g_java.activity->clazz, g_java.connect_bluetooth, text, static_cast<jint>(link));
            env->DeleteLocalRef(text);
        } else {
            static_cast<void>(::close(link));  // Out of memory: MainActivity never had it.
        }
        // MainActivity adopts the descriptor first, so it owns it even after an exception.
        static_cast<void>(cleared_exception(env));
        static_cast<void>(vm->DetachCurrentThread());
    } else {
        static_cast<void>(::close(link));
    }
}

}  // namespace

// Called by MainActivity whenever the insets of its window change. A JNI function, found by its name.
extern "C" JNIEXPORT void JNICALL Java_io_github_emrebalci_elm327controller_MainActivity_nativeSetInsets(
    JNIEnv* env, jclass clazz, jint left, jint top, jint right, jint bottom);

extern "C" JNIEXPORT void JNICALL Java_io_github_emrebalci_elm327controller_MainActivity_nativeSetInsets(
    JNIEnv* const /*env*/, const jclass /*clazz*/, const jint left, const jint top, const jint right,
    const jint bottom) {
    g_inset_left.store(left, std::memory_order_relaxed);
    g_inset_top.store(top, std::memory_order_relaxed);
    g_inset_right.store(right, std::memory_order_relaxed);
    g_inset_bottom.store(bottom, std::memory_order_relaxed);
}

namespace elm327 {
namespace gui {
namespace bridge {

common::Status begin(ANativeActivity* const activity) noexcept {
    g_java = Java{};
    JNIEnv* env{nullptr};
    common::Status status{(activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
                              ? common::Status::kOk
                              : common::Status::kSystemError};
    if (is_ok(status)) {
        g_java.activity = activity;
        g_java.env = env;
        const jclass main_activity{env->GetObjectClass(activity->clazz)};
        g_java.bluetooth_state = env->GetMethodID(main_activity, "bluetoothState", "()I");
        g_java.paired_devices = env->GetMethodID(main_activity, "pairedDevices", "()[Ljava/lang/String;");
        g_java.connect_bluetooth = env->GetMethodID(main_activity, "connectBluetooth", "(Ljava/lang/String;I)V");
        g_java.show_keyboard = env->GetMethodID(main_activity, "showKeyboard", "(Z)V");
        g_java.next_character = env->GetMethodID(main_activity, "nextCharacter", "()I");
        env->DeleteLocalRef(main_activity);
        const bool found{(g_java.bluetooth_state != nullptr) && (g_java.paired_devices != nullptr) &&
                         (g_java.connect_bluetooth != nullptr) && (g_java.show_keyboard != nullptr) &&
                         (g_java.next_character != nullptr)};
        static_cast<void>(cleared_exception(env));  // NoSuchMethodError for a missing one.
        if (!found) {
            status = common::Status::kNotSupported;
            g_java.bluetooth_state = nullptr;  // The functions below check it.
        }
    }
    return status;
}

void end() noexcept {
    sim::stop_demo();
    if (g_java.activity != nullptr) {
        static_cast<void>(g_java.activity->vm->DetachCurrentThread());
    }
    g_java = Java{};
}

BluetoothState paired_devices(const common::Span<PairedDevice> devices, std::size_t& count) noexcept {
    count = 0U;
    BluetoothState state{BluetoothState::kNoBluetooth};
    JNIEnv* const env{g_java.env};
    if (g_java.bluetooth_state != nullptr) {
        const jint answer{env->CallIntMethod(g_java.activity->clazz, g_java.bluetooth_state)};
        if (cleared_exception(env)) {
            state = BluetoothState::kNoBluetooth;
        } else if (answer == kStateReady) {
            state = BluetoothState::kReady;
        } else if (answer == kStateOff) {
            state = BluetoothState::kOff;
        } else if (answer == kStateNoPermission) {
            state = BluetoothState::kNoPermission;
        } else {
            state = BluetoothState::kNoBluetooth;
        }
    }
    if (state == BluetoothState::kReady) {
        // The names and the addresses in turn: name, address, name, address, ...
        const jobjectArray list{
            static_cast<jobjectArray>(env->CallObjectMethod(g_java.activity->clazz, g_java.paired_devices))};
        if ((!cleared_exception(env)) && (list != nullptr)) {
            const jsize length{env->GetArrayLength(list)};
            for (jsize index{0}; ((index + 1) < length) && (count < devices.size()); index += 2) {
                const jstring name{static_cast<jstring>(env->GetObjectArrayElement(list, index))};
                const jstring address{static_cast<jstring>(env->GetObjectArrayElement(list, index + 1))};
                PairedDevice& device{devices[count]};
                if (copy_string(env, address, device.address)) {
                    if ((!copy_string(env, name, device.name)) || device.name.empty()) {
                        // No name, or a very long one. Cannot fail: an address is shorter.
                        static_cast<void>(device.name.assign(device.address.view()));
                    }
                    ++count;
                }
                env->DeleteLocalRef(name);
                env->DeleteLocalRef(address);
            }
            env->DeleteLocalRef(list);
        }
    }
    return state;
}

void show_keyboard(const bool show) noexcept {
    if (g_java.show_keyboard != nullptr) {
        g_java.env->CallVoidMethod(g_java.activity->clazz, g_java.show_keyboard,
                                   static_cast<jboolean>(show ? JNI_TRUE : JNI_FALSE));
        static_cast<void>(cleared_exception(g_java.env));
    }
}

std::uint32_t next_character() noexcept {
    std::uint32_t character{0U};
    if (g_java.next_character != nullptr) {
        const jint answer{g_java.env->CallIntMethod(g_java.activity->clazz, g_java.next_character)};
        if ((!cleared_exception(g_java.env)) && (answer > 0)) {
            character = static_cast<std::uint32_t>(answer);
        }
    }
    return character;
}

Insets insets() noexcept {
    Insets insets{};
    insets.left = static_cast<float>(g_inset_left.load(std::memory_order_relaxed));
    insets.top = static_cast<float>(g_inset_top.load(std::memory_order_relaxed));
    insets.right = static_cast<float>(g_inset_right.load(std::memory_order_relaxed));
    insets.bottom = static_cast<float>(g_inset_bottom.load(std::memory_order_relaxed));
    return insets;
}

common::Result<common::NativeHandle> open_link(const char* const device, const common::Deadline& deadline,
                                               const common::NativeHandle cancel, void* const /*context*/) noexcept {
    using LinkResult = common::Result<common::NativeHandle>;
    const std::string_view name{device};
    LinkResult result{LinkResult::failure(common::Status::kNotSupported)};
    if (name == kSimulatorDevice) {
        const int stream{sim::start_demo()};
        result = (stream >= 0) ? LinkResult::success(stream) : LinkResult::failure(common::Status::kResourceLimit);
    } else if ((name.size() > kBluetoothPrefix.size()) &&
               (std::string_view{name.data(), kBluetoothPrefix.size()} == kBluetoothPrefix)) {
        std::array<int, 2U> ends{{-1, -1}};
        if (g_java.bluetooth_state == nullptr) {
            result = LinkResult::failure(common::Status::kNotSupported);
        } else if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends.data()) != 0) {
            result = LinkResult::failure(common::Status::kResourceLimit);
        } else {
            // The address follows the prefix, up to the end of the device.
            connect_bluetooth(&device[kBluetoothPrefix.size()], ends[1U]);
            const common::Status status{wait_for_link(ends[0U], cancel, deadline)};
            if (is_ok(status)) {
                result = LinkResult::success(ends[0U]);
            } else {
                // MainActivity finds the link closed and drops the Bluetooth connection.
                static_cast<void>(::close(ends[0U]));
                result = LinkResult::failure(status);
            }
        }
    } else {
        // Not a link: a serial device, e.g. a pseudo-terminal.
    }
    return result;
}

}  // namespace bridge
}  // namespace gui
}  // namespace elm327
