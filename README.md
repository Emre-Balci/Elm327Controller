# Elm327Controller

`elm327ctl` is a command-line tool for ELM327 OBD-II adapters on Linux. It reads live values,
trouble codes and the VIN from a vehicle, monitors values over time, and passes raw commands to
the adapter.

It is built on [Common](https://github.com/Emre-Balci/Common), the real-time foundation library,
which is included as the git submodule `third_party/common`. The code follows Common's rules: no
heap, no exceptions, and every wait is bounded by a deadline.

Status: version 0.1.0, the first one. See [Next steps](#next-steps).

## Quick start

```bash
git clone --recurse-submodules https://github.com/Emre-Balci/Elm327Controller.git
cd Elm327Controller
cmake --preset gcc-release && cmake --build --preset gcc-release
```

If the repository was cloned without `--recurse-submodules`, run `git submodule update --init`.

No adapter at hand? `elm327sim` simulates one on a pseudo-terminal, connected to a car that keeps
accelerating and braking:

```bash
./build/gcc-release/sim/elm327sim &      # prints e.g. "Simulated ELM327 on /dev/pts/5"
./build/gcc-release/elm327ctl -d /dev/pts/5 info
```

`elm327sim --legacy` simulates an ISO 9141-2 car instead of a CAN car, `--ignition-off` a car that
does not answer, and `--slow` the reset and protocol-search times of a real adapter.

## Usage

```
Usage: elm327ctl [options] <command> [arguments]

Commands:
  info              adapter identity, supply voltage and the vehicle's protocol
  pids              list the service 01 PIDs the vehicle supports
  read PID...       read service 01 PIDs once, e.g. 'read 0C 0D 05'
  monitor PID...    read PIDs repeatedly until Ctrl+C (see -i, -n)
  dtc               read the MIL status and the stored, pending and permanent DTCs
  vin               read the vehicle identification number
  raw COMMAND...    send AT commands or OBD requests and print the replies
  help              show this help

Options:
  -d, --device PATH   serial device (default /dev/ttyUSB0)
  -b, --baud RATE     bit rate (default 38400)
  -p, --protocol N    OBD protocol 0-9 or A-C (default 0: automatic)
  -t, --timeout MS    reply timeout of a command in ms (default 5000)
  -i, --interval MS   monitor: time between samples in ms (default 1000)
  -n, --count N       monitor: stop after N samples (default 0: until Ctrl+C)
  -v, --verbose       print the traffic with the adapter on stderr
  -h, --help          show this help
      --version       show the version
```

Examples, from the simulator:

```
$ elm327ctl info
Adapter      ELM327 v1.5
Description  OBDII to RS232 Interpreter
Voltage      12.6V
Protocol     AUTO, ISO 15765-4 (CAN 11/500)

$ elm327ctl read 0C 0D 05 42 A6 0A
0C  Engine speed                900.00 rpm
0D  Vehicle speed               4 km/h
05  Engine coolant temperature  20 degC
42  Control module voltage      14.000 V
A6  Odometer                    12345.6 km
0A  Fuel pressure               not supported by the vehicle

$ elm327ctl dtc
MIL          on, 2 confirmed DTCs
Stored       P0133 P0420
Pending      P0171
Permanent    P0420

$ elm327ctl -i 250 -n 3 monitor 0C 0D
# 0C  Engine speed [rpm]
# 0D  Vehicle speed [km/h]
  time [s]        0C        0D
     0.000   2375.00        63
     0.250   2400.00        64
     0.500   2475.00        67
elm327ctl: 3 samples, 0 failed reads

$ elm327ctl raw ATRV 0100
> ATRV
12.6V
> 0100
SEARCHING...
41 00 BE 3F 80 13
```

Every command first resets the adapter and configures it (`ATZ`, `ATE0`, `ATL0`, and `ATSP` only if
the adapter has another protocol selected, because it stores the protocol in its EEPROM). `-v`
shows that traffic. Results go to stdout; errors and the monitor summary go to stderr. The
`monitor` table is easy to parse: legend lines start with `#`, and samples keep the interval
without drifting.

Exit codes: 0 success, 1 the device, the adapter or the vehicle failed (or a PID could not be
read), 2 invalid command line, 128 + signal number when a signal stopped the program (130 for
Ctrl+C, 141 for a closed output pipe). On a signal the program finishes cleanly and closes the
serial port.

## Connecting an adapter

- USB adapters (FTDI, CH340, PL2303 chips) appear as `/dev/ttyUSB0`. Bluetooth adapters appear as
  `/dev/rfcomm0` once bound with `rfcomm`.
- Opening the device needs the `dialout` group: `sudo usermod -aG dialout "$USER"`, then log in
  again.
- Most adapters talk at 38400 bit/s, the default. If `elm327ctl` reports that the adapter does not
  answer, try `-b 9600` or `-b 115200`.
- WSL does not see USB devices by default. Attach the adapter with
  [usbipd-win](https://github.com/dorssel/usbipd-win): in an administrator PowerShell, `usbipd list`,
  then `usbipd bind --busid <id>` once and `usbipd attach --wsl --busid <id>`.
- The vehicle's ignition must be on. The first request after a reset can take several seconds,
  while the adapter searches for the vehicle's protocol (`SEARCHING...`); `-p 6` (CAN 11 bit,
  500 kbit/s, most cars since 2008) skips the search.

## How it works

| Layer | Where | What it does |
|---|---|---|
| Adapter | `include/elm327/adapter/` | `Adapter` sends a command over the serial port and collects the reply lines up to the `>` prompt, within a deadline and cancellable through a descriptor. `Response` classifies the lines (`OK`, `NO DATA`, `UNABLE TO CONNECT`, `CAN ERROR`, ...). |
| OBD-II | `include/elm327/obd/` | `parse_messages()` turns reply lines into vehicle messages, including CAN multi-frame replies. `pid.hpp` names and decodes the service 01 PIDs exactly, `dtc.hpp` and `vin.hpp` decode trouble codes and the VIN in the CAN and the older formats. `ObdClient` runs the requests. |
| CLI | `cli/` | Argument parsing, the commands, output without stdio, and `StopSignals`, which turns Ctrl+C into a readable descriptor instead of killing the process with the port still open. |
| Simulator | `sim/` | `FakeElm327`, a simulated adapter and vehicle on a pseudo-terminal: the fixture of the tests and the core of `elm327sim`. |

What Common provides:

| Need | Common component |
|---|---|
| Serial line in raw mode, exclusive access, deadlines, unplug detection | `file/serial_port.hpp` (`SerialPort`) |
| Splitting the byte stream into lines and the `>` prompt | `protocol/delimited_framer.hpp` (`DelimitedFramer`, `ByteSet`) |
| Matching messages, checked hex parsing, formatting | `protocol/text.hpp` |
| Big-endian fields and "supported PIDs" bitmaps | `protocol/byte_order.hpp` |
| One deadline per command, monotonic time | `time/deadline.hpp`, `time/clock.hpp` |
| Exact decoded values, rounded once | `numeric/decimal.hpp`, `numeric/integer.hpp` |
| Fixed-capacity storage | `containers/static_string.hpp`, `static_vector.hpp`, `span.hpp` |
| Errors without exceptions | `core/status.hpp` (`Status`, `Result<T>`) |
| Compiler policy: warnings, `-Werror`, `-fno-exceptions`, sanitizers | `common::options` |

Decoded values are exact: `decode_pid()` evaluates the formula of a PID with integers and rounds
the result once, half to even, to the resolution of the PID, so `(256 * A + B) / 4` prints as
`1726.25 rpm` and never as `1726.2499999`.

## Building and testing

Requires Linux (or WSL), CMake 3.21 or later, Ninja, and GCC 9+ or Clang 10+.

```bash
cmake --preset gcc-debug          # also: gcc-release, clang-debug, clang-release, asan, tsan, tidy
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

The tests (`build/<preset>/tests/elm327_tests [name-filter]`) cover the parsing and decoding, and
run the adapter, the OBD client and every CLI command against the simulated adapter. Run the
`asan` and `tsan` presets after changing anything that waits, reads or uses threads. The `tidy`
preset needs `sudo apt install clang-tidy`.

## Rules

The library (`include/`, `src/`) and the CLI (`cli/`) follow the rules of Common's README: no heap,
no blocking without a deadline, no exceptions, bounded loops, monotonic time, no floating-point
equality, and the MISRA C++:2023 style (fixed-width types, brace initialisation, explicit casts,
`final` classes, `[[nodiscard]]`). They add no MISRA deviations of their own; the deviations of
Common are listed in its README. The simulator and the tests use `std::string`, `std::thread` and
`<cstdio>` and are outside the compliance scope, like Common's tests and examples.

## Updating Common

`third_party/common` is pinned to a commit of Common. To move it to the latest `main`:

```bash
git -C third_party/common pull origin main
git add third_party/common
git commit -m "Update Common"
```

## Next steps

- Clear the trouble codes (service 04), behind an explicit confirmation flag.
- Freeze-frame data (service 02) and the readiness monitors of PID 01.
- Headers on (`ATH1`), to tell which ECU answered.
- Faster monitoring: several PIDs per request on CAN, and the expected reply count after a request.
- Baud-rate detection, and adapters over TCP (Wi-Fi).
- CSV output for `monitor`.
