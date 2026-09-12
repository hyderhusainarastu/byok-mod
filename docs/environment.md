# Development environment

This describes one verified-working setup for building the firmware and the
macOS host tools in this repository. It is not the only environment that
will work — it's the one this project has actually built and tested
against, recorded here so a fresh clone has a known-good target instead of
guessing at version combinations.

Host: macOS on Apple Silicon (arm64). The Xcode Command Line Tools alone are
enough — a full Xcode.app install is not required.

## Python

The system/Command Line Tools Python (`/usr/bin/python3`, currently 3.9.x)
is sufficient. ESP-IDF v5.5.3's own `tools/python_version_checker.py` sets
`OLDEST_PYTHON_SUPPORTED = (3, 9)`, so a stock CLT install meets the
minimum with no separate Python install needed. ESP-IDF also builds its own
isolated virtualenv under `~/.espressif/python_env/` during `install.sh`,
so the system Python is only used to bootstrap that, not to run the build
directly.

The macOS host tools (`host/macos/`) use their own project virtualenv
(`host/macos/.venv/`) — see [README.md](../README.md)'s "Host tools (macOS)"
section for the exact `venv` + `pip install -e '.[dev]'` steps. That venv
is independent of whichever Python built ESP-IDF's own environment.

## ESP-IDF (firmware toolchain)

| Item | Value |
|---|---|
| ESP-IDF version | **v5.5.3** (this is the version the firmware in this repo is written and tested against; newer v5.5.x point releases likely also work but haven't been tried here) |
| Target chip | `esp32s3` |
| Source location | `$IDF_PATH`, defaulting to `~/esp/esp-idf` |
| Toolchain/tools root | `~/.espressif` |
| Xtensa GCC | `xtensa-esp-elf-gcc` (crosstool-NG `esp-14.2.0_20251107`), 14.2.0 |
| cmake | 3.30.2, installed under `~/.espressif/tools/cmake/` |
| ninja | 1.12.1, installed under `~/.espressif/tools/ninja/` |
| esptool (bundled in the IDF venv) | 4.12.0 — used only as an IDF build/image-generation dependency; this project's own scripts never call it to flash or erase a device (see `SAFETY.md`) |

### Getting it

```sh
git clone --depth 1 --branch v5.5.3 --recursive \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
```

### A macOS-specific gotcha: cmake/ninja don't come down automatically

`install.sh esp32s3` only installs the target's cross toolchain, GDB, and
OpenOCD. On macOS (`macos-arm64`), ESP-IDF's own `tools/tools.json` marks
`cmake` and `ninja` as `install_type: on_request`, not `always` — they ship
automatically on some other platforms but not this one. Symptom: `idf.py
set-target esp32s3` fails with `"cmake" must be available on the PATH`.
Fix, staying inside `~/.espressif` (no Homebrew, no sudo):

```sh
python3 ~/esp/esp-idf/tools/idf_tools.py install cmake ninja
```

After that, `idf.py set-target esp32s3 && idf.py build` builds cleanly.

### Activating the environment

```sh
source ~/esp/esp-idf/export.sh
```

or, equivalently, this repo's own wrapper, which defaults `IDF_PATH` to
`~/esp/esp-idf` if it isn't already set in the environment:

```sh
source scripts/idf-env.sh
```

`scripts/idf-env.sh` respects an existing `$IDF_PATH` if you've already set
one (e.g. a different ESP-IDF checkout), and only falls back to
`~/esp/esp-idf` when `$IDF_PATH` is unset.

### Validation

A stock `examples/get-started/hello_world` build for `esp32s3` (built in a
scratch copy, never flashed, no device involved) is a reasonable sanity
check that the toolchain is wired up correctly before touching this
project's own firmware:

```sh
cd /path/to/some/scratch/copy/hello_world
source ~/esp/esp-idf/export.sh   # or: source /path/to/this/repo/scripts/idf-env.sh
idf.py set-target esp32s3
idf.py build
```

A clean run completes with no build errors and produces `hello_world.bin`.

Disk footprint: `~/esp/esp-idf` is roughly 2.3 GB, `~/.espressif` roughly
3.8 GB — plan for ~6 GB free before starting.

## Swift toolchain (macOS host helpers)

`host/macos/ScreenMirrorHelper/` and `host/macos/VirtualDisplayHelper/` are
Swift Package Manager executables built with the Swift toolchain that ships
with the Xcode Command Line Tools — no separate Swift install is needed.
This project has been built against Swift 6.3.3 (Apple's CLT toolchain,
`arm64-apple-macosx` target). Build either package with:

```sh
cd host/macos/ScreenMirrorHelper   # or VirtualDisplayHelper
swift build -c release
```

## Homebrew tools

Homebrew itself (`/opt/homebrew` on Apple Silicon, no `sudo` needed for
user-writable installs) is used for a small number of optional or
supporting tools:

| Tool | Used for |
|---|---|
| `libusb` | USB tooling support on the host side |
| `chocolate-doom` | the optional `extras/doom/` sample project only — see `docs/sample-projects/doom.md` |
| `tio` / `picocom` (optional) | a nicer serial terminal than the built-in `screen`, if wanted |

None of these are required to build the firmware or the core host package;
`screen` (built into macOS) is sufficient for passive serial listening, and
the core Python/Swift builds above don't depend on Homebrew at all.

## Firmware safety note

Building firmware never requires touching a real device. Building,
flashing to a spare/test setup, and any command that talks to a serial
port are different things — see `SAFETY.md` and `docs/hardware.md` for
what's safe to run against a real unit. Any Python invocation in this
repository that could otherwise open a real serial port should be run with
`BYOK_FORCE_MOCK=1` set unless you specifically intend to talk to a
connected device.
