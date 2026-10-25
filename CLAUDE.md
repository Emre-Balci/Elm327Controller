# Elm327Controller — notes for Claude

C++17 tools for ELM327 OBD-II adapters on Linux, built on the Common library, in two parts: the
static library `libelm327`, and the front-ends on it, `elm327ctl` (command line and interactive
shell) and `elm327gui` (Dear ImGui). See README.md for the commands, the layers and how Common is
used.

## Layout
- `lib/`: the static library. Headers in `lib/include/elm327/<group>/`, sources in
  `lib/src/<group>/`: `adapter` talks to the ELM327 over the serial port; `obd` holds the OBD-II
  services (messages, PIDs, DTCs, VIN, `ObdClient`); `controller` holds `Controller`, the
  connection the front-ends share, with `explain.hpp` (failures in words), `signals.hpp`
  (`StopSignals`) and `timing.hpp` (periodic reads). Logic both front-ends need belongs here.
- `cli/`: `elm327ctl`. Everything but `main.cpp` is the `elm327ctl_core` library, so the tests can
  run commands and the shell in-process. `commands` runs the commands over a `Controller`
  (`Session`), `shell` the interactive loop, `terminal` reads lines (raw mode on a terminal),
  `line_editor` turns keys into a line without touching the terminal.
- `gui/`: `elm327gui`. `worker` (the `elm327gui_core` library, no ImGui) owns the `Controller` on a
  thread of its own and talks to the window through Common's queues; `app` draws the window with
  ImGui; `main` sets up GLFW and OpenGL. The window never does serial I/O. Widget mode: `gauge`
  (also in `elm327gui_core`, no ImGui) holds the scale of every dial and plans the layout, so a new
  dial is a row in its table plus a test; `dashboard` draws them.
- `sim/`: `FakeElm327`, a simulated adapter and vehicle on a pseudo-terminal, and the `elm327sim`
  tool. Extend it whenever the library learns a new command, so the tests can cover it.
- `tests/<group>/`, `tests/cli/`, `tests/gui/`: tests with the same minimal framework as Common's.
- `third_party/common`: the Common library as a git submodule. Do not edit it here: changes to
  Common go to its own repository (`/mnt/d/Repos/Common`), and this repository then moves the
  submodule to the new commit.
- `third_party/imgui`: Dear ImGui as a git submodule, pinned to a release tag; `gui/CMakeLists.txt`
  builds it. Do not edit it.

A new source file is added to its directory's `CMakeLists.txt` (or `tests/CMakeLists.txt`) and,
for a new component, to the README tables.

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

The GUI is built when GLFW 3.3+ is found (`ELM327_BUILD_GUI=AUTO`): `sudo apt install
libglfw3-dev`. On this machine, without sudo, GLFW is that Ubuntu package extracted into `~/.local`,
which CMake finds through `~/.local/bin` in `PATH`.
WSLg provides the display. `elm327gui --screenshot FILE.ppm` (with `--tab`, `--monitor`,
`--widgets`) renders the window, saves it and exits, which lets a change to the window be checked
without clicking.
