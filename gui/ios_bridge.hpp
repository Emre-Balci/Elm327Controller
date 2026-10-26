/// @file ios_bridge.hpp
/// @brief What elm327gui needs on iOS from the system: Bluetooth Low Energy adapters and links to
///        them (Core Bluetooth), the app's Documents directory, and the simulator.
///
/// iOS apps cannot use the Bluetooth (classic) adapters most ELM327 clones are: Apple allows the
/// serial port profile only for accessories made for it. An iPhone reaches an adapter on Wi-Fi
/// (elm327::kTcpPrefix, which the Adapter connects to itself) or with Bluetooth Low Energy. A BLE
/// adapter offers a GATT service with a characteristic that notifies what the adapter sends, and one
/// that takes what it is sent; the usual ones are FFF0 (FFF1 notifies, FFF2 takes), FFE0 (FFE1 does
/// both) and 18F0 (2AF0, 2AF1).
///
/// scan() looks for such adapters for a while. open_link(), the LinkOpener of the Worker, opens a
/// bridged link (see elm327/controller/bridged_link.hpp) for a device "ble:<identifier>": the Core
/// Bluetooth side connects, finds the two characteristics, answers kConnected on its end of the
/// socket pair, and then writes every notification into that end and turns what it reads from it
/// into writes. Closing either side ends the link.
///
/// The simulator runs inside the app as the device "sim", on a socket pair as well.
///
/// The Core Bluetooth side runs on a dispatch queue of its own. The functions may be called from any
/// thread; open_link() runs on the worker's.

#ifndef ELM327_GUI_IOS_BRIDGE_HPP
#define ELM327_GUI_IOS_BRIDGE_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/time/deadline.hpp"
#include "gui/features.hpp"

namespace elm327 {
namespace gui {
namespace ios {

/// The device of the simulator inside the app.
inline constexpr std::string_view kSimulatorDevice{"sim"};

/// A Bluetooth Low Energy adapter that scan() found.
struct FoundDevice final {
    common::StaticString<64U> name{};        ///< E.g. "IOS-Vlink"; the identifier when it has no name.
    common::StaticString<36U> identifier{};  ///< The UUID iOS gives the device, e.g. "6B1F...".
};

/// Whether Bluetooth adapters can be found.
enum class BluetoothState : std::uint8_t {
    kReady,
    kStarting,      ///< Core Bluetooth has not told yet; ask again in a moment.
    kUnsupported,   ///< The device has no Bluetooth Low Energy.
    kOff,           ///< Bluetooth is turned off.
    kNoPermission,  ///< The app may not use Bluetooth (Settings > Privacy > Bluetooth).
};

/// Looks for Bluetooth Low Energy adapters for a while (about 15 s), from the first call on also
/// asking the person for the permission. Calling it again starts the time anew.
void scan() noexcept;

/// The adapters scan() found, into @p devices (as many as fit), and their number into @p count.
[[nodiscard]] BluetoothState found_devices(common::Span<FoundDevice> devices, std::size_t& count) noexcept;

/// The app's Documents directory, which the Files app shows. @return kOk; kNotFound.
[[nodiscard]] common::Status documents_directory(Path& directory) noexcept;

/// The LinkOpener of the Worker (see Controller::set_link_opener()): "ble:<identifier>" connects to
/// the Bluetooth Low Energy adapter, waiting at most until @p deadline and until @p cancel is
/// readable, and "sim" starts the simulator. @p context is unused.
/// @return the stream to the adapter; kNotSupported for any other device (e.g. "tcp:...", which the
///         Adapter opens); otherwise the errors of open_bridged_link().
[[nodiscard]] common::Result<common::NativeHandle> open_link(const char* device, const common::Deadline& deadline,
                                                             common::NativeHandle cancel, void* context) noexcept;

}  // namespace ios
}  // namespace gui
}  // namespace elm327

#endif  // ELM327_GUI_IOS_BRIDGE_HPP
