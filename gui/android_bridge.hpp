/// @file android_bridge.hpp
/// @brief What elm327gui needs on Android from the Java side of the app, MainActivity: the paired
///        Bluetooth adapters and links to them, the on-screen keyboard, and the screen's insets.
///
/// An Android app reaches Bluetooth only through the Java API: a Bluetooth adapter speaks the serial
/// port profile (RFCOMM), and MainActivity connects a BluetoothSocket to it. open_link(), the
/// LinkOpener of the Worker, opens a bridged link (see elm327/controller/bridged_link.hpp):
/// MainActivity gets one end of a socket pair with the address, connects, answers its status byte,
/// and copies the bytes between the BluetoothSocket and that end; the Adapter reads and writes the
/// other end. When the Adapter closes its end, MainActivity closes the Bluetooth connection, and
/// when the adapter goes away, the Adapter finds its end closed (kDisconnected).
///
/// The simulator runs inside the app as the device "sim", on a socket pair as well, so the app can
/// be tried without a car.
///
/// The functions talk to Java through JNI. Only open_link() runs on the worker's thread; the others
/// run on the thread of android_main(), between begin() and end().

#ifndef ELM327_GUI_ANDROID_BRIDGE_HPP
#define ELM327_GUI_ANDROID_BRIDGE_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/time/deadline.hpp"

struct ANativeActivity;

namespace elm327 {
namespace gui {
namespace bridge {

/// The device of the simulator inside the app. (A Bluetooth adapter is elm327::kBluetoothPrefix and
/// its address, e.g. "bt:00:1D:A5:68:98:8B".)
inline constexpr std::string_view kSimulatorDevice{"sim"};

/// A Bluetooth device paired with the phone in Android's settings.
struct PairedDevice final {
    common::StaticString<64U> name{};     ///< E.g. "OBDII"; the address when it has no name.
    common::StaticString<17U> address{};  ///< E.g. "00:1D:A5:68:98:8B".
};

/// Whether the paired devices can be listed.
enum class BluetoothState : std::uint8_t {
    kReady,
    kNoBluetooth,   ///< The phone has no Bluetooth.
    kOff,           ///< Bluetooth is turned off.
    kNoPermission,  ///< The app may not connect to nearby devices (Android 12 and later): asked for.
};

/// The insets of the screen in pixels: what the status bar, the navigation bar and a display cutout
/// cover, which the window keeps clear of.
struct Insets final {
    float left{0.0F};
    float top{0.0F};
    float right{0.0F};
    float bottom{0.0F};
};

/// Starts the bridge on the thread of android_main(): attaches it to Java and finds the methods of
/// MainActivity. @return kOk; kNotSupported if the activity lacks one of them.
common::Status begin(ANativeActivity* activity) noexcept;

/// Detaches the thread of android_main() from Java again. The simulator stops.
void end() noexcept;

/// Lists the paired Bluetooth devices into @p devices (as many as fit), and their number into
/// @p count. Without the permission, asks the person for it; the next call then finds them.
[[nodiscard]] BluetoothState paired_devices(common::Span<PairedDevice> devices, std::size_t& count) noexcept;

/// Shows the on-screen keyboard, or hides it.
void show_keyboard(bool show) noexcept;

/// @return the next character typed on the on-screen keyboard, as a Unicode code point; 0 when there
///         is none.
[[nodiscard]] std::uint32_t next_character() noexcept;

/// @return the insets MainActivity last reported.
[[nodiscard]] Insets insets() noexcept;

/// The LinkOpener of the Worker (see Controller::set_link_opener()): "bt:<address>" connects to the
/// Bluetooth adapter through MainActivity, waiting at most until @p deadline or until @p cancel is
/// readable, and "sim" starts the simulator. @p context is unused.
/// @return the stream to the adapter; kNotSupported for any other device; kNotFound (no such
///         device), kPermissionDenied (no permission for nearby devices), kNotRunning (Bluetooth
///         off), kDisconnected (the adapter refused the connection), kTimeout or kInterrupted.
[[nodiscard]] common::Result<common::NativeHandle> open_link(const char* device, const common::Deadline& deadline,
                                                             common::NativeHandle cancel, void* context) noexcept;

}  // namespace bridge
}  // namespace gui
}  // namespace elm327

#endif  // ELM327_GUI_ANDROID_BRIDGE_HPP
