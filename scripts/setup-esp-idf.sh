#!/usr/bin/env bash
# setup-esp-idf.sh — one-time bootstrap of the ESP-IDF toolchain this firmware
# builds against.
#
# This script never touches a device. It only clones ESP-IDF, installs the
# esp32s3 cross toolchain, and (on macOS, where they are not installed by
# default) adds cmake and ninja. Nothing here flashes, erases, or opens a
# serial port.
#
# Usage:
#   scripts/setup-esp-idf.sh              # install to ${IDF_PATH:-~/esp/esp-idf}
#   IDF_PATH=/other/place scripts/setup-esp-idf.sh
#
# Then, in each shell you want to build from:
#   source scripts/idf-env.sh
#   cd firmware/s3 && idf.py set-target esp32s3 && idf.py build
#
# Disk footprint: the ESP-IDF checkout is roughly 2.3 GB and the tools root
# (~/.espressif) roughly 3.8 GB — plan for about 6 GB free.
#
# See docs/environment.md for the full, verified-working environment this
# reproduces, including the macOS cmake/ninja gotcha handled below.

set -euo pipefail

IDF_VERSION="v5.5.3"
IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf}"
TARGET="esp32s3"

say() { printf '==> %s\n' "$*"; }
die() { printf 'setup-esp-idf.sh: %s\n' "$*" >&2; exit 1; }

command -v git >/dev/null 2>&1 || die "git is required but not on PATH"
command -v python3 >/dev/null 2>&1 || die "python3 is required but not on PATH"

if [ -d "$IDF_DIR/.git" ]; then
    say "ESP-IDF already present at $IDF_DIR — leaving it alone"
    say "    (checked-out version: $(git -C "$IDF_DIR" describe --tags --always 2>/dev/null || echo unknown))"
else
    [ -e "$IDF_DIR" ] && die "$IDF_DIR exists but is not a git checkout; move it aside first"
    say "Cloning ESP-IDF $IDF_VERSION into $IDF_DIR (this takes a while)"
    mkdir -p "$(dirname "$IDF_DIR")"
    git clone --depth 1 --branch "$IDF_VERSION" --recursive \
        https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi

say "Installing the $TARGET cross toolchain (into \$IDF_TOOLS_PATH, default ~/.espressif)"
"$IDF_DIR/install.sh" "$TARGET"

# On macos-arm64, ESP-IDF's own tools.json marks cmake and ninja as
# install_type "on_request", so install.sh does not fetch them and
# `idf.py set-target` then fails with '"cmake" must be available on the PATH'.
# Installing them explicitly is a no-op where they are already present.
if [ "$(uname -s)" = "Darwin" ]; then
    say "macOS: installing cmake and ninja explicitly (not installed by install.sh on this platform)"
    python3 "$IDF_DIR/tools/idf_tools.py" install cmake ninja
fi

cat <<MSG

Done. ESP-IDF $IDF_VERSION is installed at:
    $IDF_DIR

Activate it in a shell with this repo's wrapper (it respects an existing
\$IDF_PATH and otherwise defaults to ~/esp/esp-idf):

    source scripts/idf-env.sh

Then build the firmware — this compiles only, and flashes nothing:

    cd firmware/s3
    idf.py set-target esp32s3
    idf.py build

Do not run 'idf.py flash' against the device. See SAFETY.md and
docs/firmware.md for the one supported install path, which goes through the
stock SD-card updater.
MSG
