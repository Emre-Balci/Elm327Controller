# Elm327Controller — notes for Claude

C++17 tools for ELM327 OBD-II adapters on Linux, Windows (MSVC) and Android, built on the Common
library, in two parts: the static library `libelm327`, and the front-ends on it, `elm327ctl`
(command line and interactive shell) and `elm327gui` (Dear ImGui; on Android, an app). See README.md
for the commands, the layers and how Common is used.

## Layout
- `lib/`: the static library. Headers in `lib/include/elm327/<group>/`, sources in
  `lib/src/<group>/`: `adapter` talks to the ELM327 over the serial port; `obd` holds the OBD-II
  services (messages, PIDs, DTCs, VIN, `ObdClient` with `clear_dtcs()`) and `service.hpp`, which
  classifies a request as read or write for the read-only gate; `expert` holds `uds.hpp` (building
  and decoding UDS/KWP requests), `feature.hpp` (feature definitions, and `FeatureCoder`, which
  turns a feature on and off: identification and length checks, backup, session, write, verify,
  restore) and `coding.hpp` (a reference map for VAG/Renault/BMW, no car-specific values);
  `controller` holds `Controller`, the connection the front-ends share, with
  its `AccessPolicy` (the read-only safety mode, enforced in `execute()` and `clear_dtcs()`),
  `explain.hpp` (failures in words), `signals.hpp` (`StopSignals`) and `timing.hpp` (periodic
  reads). Logic both front-ends need belongs here.
- Read-only mode and expert mode: the gate lives in `obd/service.hpp` (`command_access()`,
  default-deny) and `Controller` (`set_access_policy()`), so every front-end -- `raw`, `clear`,
  `uds`, the GUI terminal -- is covered at one choke point. Expert mode is the UDS/KWP toolkit in
  `expert/`; keep it protocol-only, with ECU-specific data left to the user, and never add odometer,
  immobiliser or emissions-defeat functions. Feature definitions are data, too: the only built-in
  ones are the simulator's (`gui/features.cpp`, matching its "SIM-" part numbers). Add definitions
  for a real car only when they were verified on it, with its part number in `identify`.
- `cli/`: `elm327ctl`. Everything but `main.cpp` is the `elm327ctl_core` library, so the tests can
  run commands and the shell in-process. `commands` runs the commands over a `Controller`
  (`Session`), `shell` the interactive loop, `terminal` reads lines (raw mode on a terminal),
  `line_editor` turns keys into a line without touching the terminal.
- `gui/`: `elm327gui`. `worker` (the `elm327gui_core` library, no ImGui) owns the `Controller` on a
  thread of its own and talks to the window through Common's queues; `app` draws the window with
  ImGui; `main` sets up GLFW and OpenGL. The window never does serial I/O. The Expert tab posts UDS
  requests (`RequestKind::kUds`, and `kUdsWrite`, which backs the value up first and writes nothing
  without that backup) and gets a `UdsResult` for each through a queue of its own; every request it
  can send that changes the vehicle goes through its confirmation dialog. The Features tab posts
  `kFeatureRead`, `kFeatureChange` and `kFeatureRestore` with the definition by value and gets a
  `FeatureResult` each through another queue; `features` (in `elm327gui_core`) holds the built-in
  definitions, loads the user's file and appends every change to `backups.txt` in
  `~/.config/elm327controller` (found with `getpwuid_r`, as MISRA bans `getenv`; on Windows
  `%APPDATA%\elm327controller`, the known folder from `SHGetKnownFolderPath`; on Android the app's
  directory on the shared storage, which `main_android` passes to `set_config_directory()`). Widget mode:
  `gauge` (also in `elm327gui_core`, no ImGui) holds the scale of every dial and plans the layout,
  so a new dial is a row in its table plus a test; `dashboard` draws them. On Android,
  `main_android` (the native activity's `android_main()`, OpenGL ES 3) replaces `main`, and
  `android_bridge` is the JNI side of what only Java reaches (see Platforms).
- `sim/`: `FakeElm327`, a simulated adapter and vehicle on a pseudo-terminal (on Windows, a named
  pipe, which `Adapter` opens in place of a COM port), and the `elm327sim` tool. It answers OBD-II
  (service 01/03/04/07/09/0A) and a subset of UDS (sessions, read/write data by identifier, routine,
  security, tester present, reset). The engine module answers the default
  header with `Vehicle::data_by_identifier`; `Vehicle::modules` adds modules at their own header
  (ATSH), e.g. the cluster at 714, and any other header gets "NO DATA". Writes to
  `session_protected_dids` need the extended session, and `locked_dids` refuse them (security
  access). `did_value()` lets a test check what a write did, and that in read-only mode none
  arrived. Extend it whenever the library learns a new command, so the tests can cover it.
  `Transport::kSocket` serves on a socket pair instead, whose other end goes to
  `Adapter::open_stream()`. `demo` drives the car of `elm327sim` and runs it as the Android app's
  demo device (`start_demo()`).
- `android/`: the Android app, a Gradle project (Kotlin DSL, Java sources) whose native code is the
  root `CMakeLists.txt` (target `elm327gui`, a shared library). `MainActivity` is the
  `NativeActivity`; `BluetoothLink` connects a paired adapter and copies between its
  `BluetoothSocket` and the native side's socket pair. The JNI method names and the status codes
  are in step with `gui/android_bridge.cpp`: change both together.
- `tests/<group>/`, `tests/cli/`, `tests/gui/`: tests with the same minimal framework as Common's.
  `test_support.hpp` has what they need from the OS (cancel events, captured output, pipes), with
  its OS code in `test_support.cpp`.
- `third_party/common`: the Common library as a git submodule. Do not edit it here: changes to
  Common go to its own repository (`/mnt/d/Repos/Common`), and this repository then moves the
  submodule to the new commit.
- `third_party/imgui`: Dear ImGui as a git submodule, pinned to a release tag; `gui/CMakeLists.txt`
  builds it. Do not edit it.

A new source file is added to its directory's `CMakeLists.txt` (or `tests/CMakeLists.txt`) and,
for a new component, to the README tables.

## Platforms
Linux and Windows (MSVC), like Common. Platform code sits in `#if defined(_WIN32)` / `#else` blocks
next to the POSIX code, so both stay in step; a component that is mostly platform-specific has
`<name>.cpp` (shared parts), `<name>_posix.cpp` and `<name>_win32.cpp`, chosen in its
`CMakeLists.txt`: the adapter's I/O (`adapter_*`), the stop signals (`signals_*`) and the shell's
terminal (`terminal_*`). Descriptors are `common::NativeHandle`: an eventfd or a signalfd on Linux,
a manual-reset event on Windows, and `common::kInvalidNativeHandle` for none.
- Public headers never include `<windows.h>`. CMake defines `NOMINMAX` and `WIN32_LEAN_AND_MEAN` for
  every target. Test headers stay free of it too, since it defines macros such as `near`: their OS
  code goes into `tests/test_support.cpp`.
- On Windows, `Adapter` opens a COM port through `common::SerialPort` and reads it with overlapped
  I/O that also ends at the cancel event; it opens a named pipe (`\\.\pipe\...`) itself, which is
  how `elm327sim` and the tests reach it. Console control events stand in for the stop signals:
  Ctrl+C is SIGINT, Ctrl+Break SIGBREAK, closing the console window SIGTERM.
- The shell's console runs in raw mode with VT input and output, so `LineEditor` gets the same
  escape sequences as on a terminal.
- Keep the Linux behaviour unchanged when adding Windows code, and say in the header what differs.

Android (API level 28+, 64-bit only, Common's requirements) is Linux with bionic: it takes the POSIX
code, and `#if defined(__ANDROID__)` marks what differs (the texts of `explain.cpp`, the GUI's
configuration directory, `test_support.cpp`). An app reaches Bluetooth only through Java, so the GUI's
`Worker` gets a `LinkOpener` (`Controller::set_link_opener()`): for a device `bt:<address>`,
`android_bridge` hands `MainActivity` one end of a socket pair, waits for its status byte (bounded by
`Timeouts::link` and the cancel descriptor), and gives the other end to `Adapter::open_stream()`,
which reads and writes a connected stream socket like a serial device (POSIX only). Writes use
`MSG_NOSIGNAL`, so a closed peer is `kDisconnected`, not SIGPIPE.

## Rules for the library, the CLI and the GUI's own code (`lib/`, `cli/`, `gui/`)
The rules of Common's CLAUDE.md apply:
- No heap allocation: no `new`, `std::vector`, `std::string`, `std::function`. Use `StaticString`,
  `StaticVector`, `Span`; capacity is a compile-time constant.
- Never wait without a deadline: serial I/O goes through `Adapter`, which takes a `Deadline` and
  watches the cancel descriptor. The only waits without one are for the user's input (the shell)
  and for the next request (the GUI's worker), and a descriptor can always end them.
- No exceptions: everything is `noexcept`; errors are `Status`/`Result<T>`. Don't call throwing std
  members (`string_view::substr`, `vector::at`, `optional::value`).
- Every loop has an explicit bound. Time comes from `MonotonicClock` only.
- No floating point for decoded values: they are exact `Measurement`s (`Decimal<3>`). The GUI
  converts them to `float` only to plot them.
- MISRA C++:2023 style: fixed-width types, brace-init, explicit casts, `final` classes,
  `[[nodiscard]]`, braces on every branch, no C-style arrays, no `<cstdio>`, and characters are
  never converted to numbers (convert through lookup tables, as Common's `text.hpp` does).
- Match the existing style: 4-space indent, 120 columns, `lower_case` functions, `CamelCase` types,
  `kConstant`, `member_`, `///` comments that say what and why.
- Types that go through Common's queues (`SpscQueue`, `TripleBuffer`) must be nothrow
  default-constructible: initialize a `Duration` member from a `constexpr` constant, since the
  converting constructor of `std::chrono::duration` is not `noexcept`.

ImGui and GLFW allocate and are compiled without this project's warnings; their headers are system
headers. In GUI code, pass text to ImGui with `TextUnformatted()` (no printf formats) and never pass
a `float` to a variadic function (`-Wdouble-promotion`).

The simulator and the tests are outside the compliance scope and may use the standard library
freely, but they build with the same warnings.

## Build & test
Build in WSL (Ubuntu-24.04). When calling `wsl` from Git Bash on Windows, set `MSYS_NO_PATHCONV=1`.

```bash
git submodule update --init   # once, after cloning
cmake --preset gcc-debug && cmake --build --preset gcc-debug && ctest --preset gcc-debug
```
Before finishing a change, run `gcc-release`, `clang-release`, `asan` and `tsan` too. All of them
must build warning-free (`-Werror`) and pass. Try a change by hand with `elm327sim`: `elm327ctl -d
<its device>` (the shell), `elm327ctl -d <device> <command>`, and `elm327gui -d <device>`.

For Windows, run `msvc-debug`, `msvc-release` and `msvc-asan` (also warning-free, `/WX`). From WSL,
put the commands in a `.bat` file that first calls `vcvars64.bat` of Visual Studio 2022 (find it
with `vswhere.exe -latest -property installationPath`, under `VC\Auxiliary\Build`), then run it
with `cmd.exe /c` from a directory on a `/mnt/<drive>` path; the built `.exe` files also run directly
from WSL. `msvc-release` matters: MSVC 19.40 has miscompiled code at `/O2` that passed in Debug. The
GUI needs a GLFW for MSVC: configure with `-DCMAKE_PREFIX_PATH=<its install directory>`. The
`*_on_a_console` tests run elm327ctl in a pseudo console, and `elm327gui --screenshot` works there
too. Never open a real COM port in a test.

For Android, set `ANDROID_NDK_HOME` to the NDK r29 and build `android-arm64` and `android-x86_64`
(also warning-free; both include the GUI's shared library), and run `ctest --preset android-x86_64`:
its executables are static, so the tests run on the Linux host with bionic. Then build the app:
`cd android && ./gradlew assembleDebug lintDebug` with `JAVA_HOME` (JDK 17+) and `ANDROID_HOME` set.
There is no emulator here (no KVM access): the app itself is untested on a device.

The GUI is built when GLFW 3.3+ is found (`ELM327_BUILD_GUI=AUTO`): `sudo apt install
libglfw3-dev`. On this machine, without sudo, GLFW is that Ubuntu package extracted into `~/.local`,
which CMake finds through `~/.local/bin` in `PATH`.
WSLg provides the display. `elm327gui --screenshot FILE.ppm` (with `--tab`, `--monitor`,
`--widgets`) renders the window, saves it and exits, which lets a change to the window be checked
without clicking.
