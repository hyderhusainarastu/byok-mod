# tests/ — how to run everything

This covers the four automated test surfaces in the repo. None of these
touch the device (no serial port, no `esptool --port`, no `idf.py flash`/
`monitor`) — they build and test host binaries/scripts and, separately,
cross-compile the S3 firmware to a local `.bin` that is inspected but never
flashed. `tests/hardware/CHECKLIST.md` is a separate, owner-performed,
fillable checklist for on-device tests and is not part of this automated
suite.

## 1. Firmware build (compile-only, no flashing)

Builds `firmware/s3` for the esp32s3 target with ESP-IDF. This is a build
check, not a test suite with pass/fail assertions — success means the tree
compiles and links against the device's actual partition table
(`firmware/s3/partitions.csv`, which reproduces the physical device's table;
see the comment at the top of that file). It does **not** flash anything.

```sh
source scripts/idf-env.sh        # or: source ~/esp/esp-idf/export.sh
cd firmware/s3
idf.py set-target esp32s3        # first time only, or after a clean
idf.py build
```

Expect a clean build (0 warnings, 0 errors) ending in a `check_sizes.py`
line comparing `byok_mod_s3.bin` against the `ota_1` slot (3 MiB /
`0x300000`), e.g.:

```
byok_mod_s3.bin binary size 0x53100 bytes. Smallest app partition is 0x300000 bytes. 0x2acf00 bytes (89%) free.
```

To sanity-check the resulting image without a device attached (local file
only — never pass `--port`):

```sh
python3 -m esptool image_info --version 2 build/byok_mod_s3.bin
```

Confirm `Chip ID: 9 (ESP32-S3)`, a valid checksum/hash, and an
`Application information` block with the expected project name and
`CONFIG_BYOK_ALLOW_OTADATA_WRITE`-gated behavior unchanged (that Kconfig
option stays `n` by default — see `firmware/s3/sdkconfig.defaults` and
`firmware/s3/main/Kconfig.projbuild`; nothing in this build step, or in any
fix applied to make it compile, should flip that default or add a new
flash-write code path).

`idf.py flash` and `idf.py monitor` are printed as suggestions at the end of
a successful build — **never run them** against the physical device from an
automated/CI context; those are owner-performed, one-at-a-time actions per
`SAFETY.md` §3.

## 2. `byok_proto` C test suite (protocol codec, host-native)

Builds and runs `tests/proto/test_proto.c` against
`firmware/common/byok_proto`'s actual sources with plain `clang`
(no ESP-IDF, no CMake) under AddressSanitizer + UndefinedBehaviorSanitizer,
and regenerates `tests/proto/vectors.json` from the C encoder's real output.

```sh
make -C tests/proto test
```

Covers: the six spec vectors from `docs/protocol.md` §12, round-trip
encode/decode for all 31 message types, malformed-length/oversized-length/
bad-CRC/bad-version rejection, truncated/split-across-many-`feed()`-calls
framing, duplicate-SEQ and SEQ-gap tracking, and a 1,000,000-iteration
random-byte fuzz pass. All of it must print `ALL TESTS PASSED`.

`vectors.json` is a generated artifact (rebuilt by every `make test` run) —
`tests/host/test_proto.py`'s cross-check tests load it to confirm the Python
reference implementation reproduces the C implementation's output
byte-for-byte, not just that both independently match the spec.

`make -C tests/proto clean` removes the built binary, its `.dSYM`, and
`vectors.json`.

## 3. Python host test suite

Covers `host/macos/byok`: the wire protocol (Python side, cross-checked
against vectors.json from step 2 above), USB-CDC transport/discovery/
reconnect logic, dashboard rendering (layout math, widgets, quantization,
dithering), and the QR encoder.

Uses the project venv at `host/macos/.venv`, which already has
`host/macos/pyproject.toml`'s dependencies (`pyserial`, `Pillow`) installed:

```sh
source host/macos/.venv/bin/activate
python3 -m unittest discover -s tests/host
```

Run `make -C tests/proto test` first (or at least once) if `test_proto.py`'s
cross-check tests are part of the run — they need `tests/proto/vectors.json`
to exist.

Expect all tests to pass. One test is expected to `skip` in this
environment (`test_pyyaml_and_fallback_parser_agree` — PyYAML is not
installed in the venv; the dashboard config loader's hand-written fallback
parser is exercised and cross-checked against PyYAML's output only when both
are importable, and the fallback parser has its own direct tests that always
run).

For a single module, or verbose output:

```sh
python3 -m unittest tests.host.test_proto -v
python3 -m unittest tests.host.test_transport -v
python3 -m unittest tests.host.test_render -v
python3 -m unittest tests.host.test_dashboard -v
```

## 4. Static stack-budget guard (host-only, no ESP-IDF needed)

`tests/static/check_stack_budgets.py` (also runnable as
`tests/static/check_stack_budgets.sh`, a thin wrapper) is the static
regression guard for
`docs/troubleshooting.md` — a FreeRTOS stack overflow
on task `main` that only ever showed up as a real-hardware boot-loop
(FreeRTOS's own stack-overflow canary can't be reproduced by a host-side
script). It instead enforces, by inspection of the committed source, the
exact conditions that made that crash possible:

```sh
python3 tests/static/check_stack_budgets.py
```

Checks: `byok_sd_updater_check_and_run()` is never called directly from
`app_main()`'s own body (it must go through the dedicated
`byok_sd_upd` task); that task's stack constant
(`SD_UPDATER_TASK_STACK_BYTES`) is at least 8192 B; neither
`byok_sd_updater.c` nor `byok_tar.c` declares a plain stack-local
`uint8_t`/`char` array over 1024 B (the fix moved both of the incident's
buffers to `heap_caps_malloc`); and `firmware/s3/sdkconfig.defaults` pins
`CONFIG_ESP_MAIN_TASK_STACK_SIZE >= 8192` and
`CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y`. No dependencies beyond
Python 3's standard library — does not need the `host/macos/.venv` this
step 3's `pytest`/`unittest` do, and does not need ESP-IDF like step 1
does. Prints `ALL CHECKS PASSED` and exits 0 on success; exits 1 with a
specific per-check message identifying the offending file on the first
failure. Would have failed against the pre-0.1.3 tree; passes against the
fixed one.

## Running all four together

```sh
# 1. Firmware (compile-only — never flash)
source scripts/idf-env.sh
(cd firmware/s3 && idf.py set-target esp32s3 && idf.py build)

# 2. Protocol C suite (regenerates vectors.json for step 3)
make -C tests/proto test

# 3. Python host suite
source host/macos/.venv/bin/activate
python3 -m unittest discover -s tests/host

# 4. Static stack-budget guard
python3 tests/static/check_stack_budgets.py
```

## What's not covered here

- `tests/hardware/CHECKLIST.md` — a manual, on-device checklist, performed
  by hand. Not automated and not run by any command above; every step in it
  is a physical action (see `SAFETY.md` §3).
- `tests/patterns/*.pbm` — fixture bitmaps consumed by the render/dashboard
  tests above, not independently tested.
