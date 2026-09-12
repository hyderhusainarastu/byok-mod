#!/usr/bin/env bash
# Source this file to activate the ESP-IDF v5.5.3 toolchain installed under
# ~/esp/esp-idf and ~/.espressif for building (NOT flashing) esp32s3 firmware.
#
# Usage:
#   source scripts/idf-env.sh
#
# After sourcing, `idf.py`, `xtensa-esp32s3-elf-gcc`, `cmake`, and `ninja`
# are on PATH. See docs/environment.md for install details and gotchas
# (cmake/ninja are "on_request" on macOS and were installed separately via
# `idf_tools.py install cmake ninja`).

IDF_INSTALL_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"

if [ ! -f "$IDF_INSTALL_PATH/export.sh" ]; then
  echo "idf-env.sh: could not find $IDF_INSTALL_PATH/export.sh" >&2
  echo "idf-env.sh: expected ESP-IDF v5.5.3 cloned at ~/esp/esp-idf (see docs/environment.md)" >&2
  return 1 2>/dev/null || exit 1
fi

# shellcheck disable=SC1090
source "$IDF_INSTALL_PATH/export.sh"
