# Elm327Controller — notes for Claude

`elm327ctl`, a C++17 command-line controller for ELM327 OBD-II adapters on Linux, built on the
Common library. See README.md for the commands, the layers and how Common is used.

## Layout
- `include/elm327/<group>/` and `src/<group>/`: the library. `adapter` talks to the ELM327 over the
  serial port; `obd` holds the OBD-II services (messages, PIDs, DTCs, VIN, `ObdClient`).
- `cli/`: the `elm327ctl` tool. Everything but `main.cpp` is the `elm327ctl_core` library, so the
  tests can run commands in-process.
- `sim/`: `FakeElm327`, a simulated adapter and vehicle on a pseudo-terminal, and the `elm327sim`
  tool. Extend it whenever the library learns a new command, so the tests can cover it.
- `tests/<group>/`, `tests/cli/`: tests with the same minimal framework as Common's.
- `third_party/common`: the Common library as a git submodule. Do not edit it here: changes to
  Common go to its own repository (`/mnt/d/Repos/Common`), and this repository then moves the
  submodule to the new commit.

A new source file is added to `CMakeLists.txt` (or `tests/CMakeLists.txt`) and, for a new
component, to the README tables.

## Rules for the library and the CLI (`include/`, `src/`, `cli/`)
The rules of Common's CLAUDE.md apply:
- No heap allocation: no `new`, `std::vector`, `std::string`, `std::function`. Use `StaticString`,
  `StaticVector`, `Span`; capacity is a compile-time constant.
- Never wait without a deadline: serial I/O goes through `Adapter`, which takes a `Deadline` and
  watches the cancel descriptor.
- No exceptions: everything is `noexcept`; errors are `Status`/`Result<T>`. Don't call throwing std
  members (`string_view::substr`, `vector::at`, `optional::value`).
- Every loop has an explicit bound. Time comes from `MonotonicClock` only.
- No floating point for decoded values: they are exact `Measurement`s (`Decimal<3>`).
- MISRA C++:2023 style: fixed-width types, brace-init, explicit casts, `final` classes,
  `[[nodiscard]]`, braces on every branch, no C-style arrays, no `<cstdio>`, and characters are
  never converted to numbers (convert through lookup tables, as Common's `text.hpp` does).
- Match the existing style: 4-space indent, 120 columns, `lower_case` functions, `CamelCase` types,
  `kConstant`, `member_`, `///` comments that say what and why.

The simulator and the tests are outside the compliance scope and may use the standard library
freely, but they build with the same warnings.

## Build & test
Build in WSL (Ubuntu-24.04). When calling `wsl` from Git Bash on Windows, set `MSYS_NO_PATHCONV=1`.

```bash
git submodule update --init   # once, after cloning
cmake --preset gcc-debug && cmake --build --preset gcc-debug && ctest --preset gcc-debug
```
Before finishing a change, run `gcc-release`, `clang-release`, `asan` and `tsan` too. All of them
must build warning-free (`-Werror`) and pass. Try a change by hand with `elm327sim` and
`elm327ctl -d <its device>`.
