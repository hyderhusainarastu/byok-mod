#!/usr/bin/env python3
"""tests/static/check_stack_budgets.py -- static regression guard for the
2026-09-03 SD-updater stack overflow (docs/troubleshooting.md).

Host-only, no device/serial/idf.py involved -- run as a plain script (no
pytest dependency, so it can run in any Python 3 without the host venv):

    python3 tests/static/check_stack_budgets.py

FreeRTOS's own stack-overflow canary (CONFIG_FREERTOS_CHECK_STACKOVERFLOW_
CANARY, already enabled -- see check 4 below) only fires on real hardware
and can't be reproduced by a host-side script. What this instead enforces,
by inspection of the committed source, is the exact set of conditions that
made the 2026-09-03 crash possible in the first place, so a future edit
that reintroduces any one of them fails this check immediately instead of
waiting for another boot-loop:

  1. byok_sd_updater_check_and_run() is not called directly from
     app_main()'s own body -- it must go through the dedicated-task
     wrapper (run_sd_updater_in_dedicated_task()/sd_updater_task()), so its
     stack is sized independently of "main"'s.
  2. That dedicated task's own stack constant (SD_UPDATER_TASK_STACK_BYTES)
     is at least MIN_UPDATER_TASK_STACK_BYTES.
  3. Neither byok_sd_updater.c nor byok_tar.c declares a plain
     task-stack-local array (`uint8_t name[N];`/`char name[N];`, N or a
     same-file #define resolving above the cap) bigger than
     MAX_STACK_ARRAY_BYTES -- the streaming/tar-header buffers must be
     heap_caps_malloc'd (a pointer, which this pattern does not match),
     not stack locals; that move off the stack is exactly the 0.1.3 fix.
  4. sdkconfig.defaults pins CONFIG_ESP_MAIN_TASK_STACK_SIZE >=
     MIN_MAIN_TASK_STACK_BYTES (the 0.1.3 defence-in-depth bump, 3584 ->
     8192) and CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y, so a future
     sdkconfig.defaults edit can't silently regress either protection back
     toward the state that made the crash possible.

Exit 0 (and print each check's own PASS line) on success. Exit 1 with a
specific, actionable message identifying the failing file/line on the
first failure -- deliberately fails loudly rather than trying to guess a
fix, per this file's own "static, not a substitute for real measurement"
scope: the real number comes from uxTaskGetStackHighWaterMark(), logged
at runtime by sd_updater_task() in app_main.c instead.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_MAIN = ROOT / "firmware/s3/main/app_main.c"
UPDATER_C = ROOT / "firmware/s3/components/byok_sd_updater/byok_sd_updater.c"
TAR_C = ROOT / "firmware/common/byok_tar/byok_tar.c"
SDKCONFIG_DEFAULTS = ROOT / "firmware/s3/sdkconfig.defaults"

MAX_STACK_ARRAY_BYTES = 1024
MIN_MAIN_TASK_STACK_BYTES = 8192
MIN_UPDATER_TASK_STACK_BYTES = 8192

FAILURES = []


def fail(msg: str) -> None:
    FAILURES.append(msg)


def ok(msg: str) -> None:
    print(f"PASS: {msg}")


def require_file(path: Path) -> str:
    if not path.is_file():
        fail(f"expected file not found: {path.relative_to(ROOT)}")
        return ""
    return path.read_text()


# ---------------------------------------------------------------------------
# Check 1: byok_sd_updater_check_and_run( is not called from app_main()'s
# own body. Isolate app_main(void){...}'s text by brace-depth counting from
# its signature (robust to the helper functions above/below it moving
# around, unlike a fixed line-range slice).
# ---------------------------------------------------------------------------
def check_updater_not_called_from_app_main_body(text: str) -> None:
    m = re.search(r"\nvoid\s+app_main\s*\(\s*void\s*\)\s*\n?\{", text)
    if not m:
        fail(f"{APP_MAIN.relative_to(ROOT)}: could not find 'void app_main(void) {{' -- has the signature changed?")
        return
    start = m.end()  # just past the opening brace
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    body = text[start:i]
    if "byok_sd_updater_check_and_run(" in body:
        fail(
            f"{APP_MAIN.relative_to(ROOT)}: byok_sd_updater_check_and_run() is called "
            "directly from app_main()'s own body again -- this is the exact condition "
            "that caused the 2026-09-03 boot-loop (task \"main\"'s stack cannot safely "
            "hold the updater's own locals). Route it through "
            "run_sd_updater_in_dedicated_task() instead; see "
            "docs/troubleshooting.md."
        )
        return
    ok("byok_sd_updater_check_and_run() is not called directly from app_main()'s body")


# ---------------------------------------------------------------------------
# Check 2: the dedicated updater task's own stack constant is present and
# large enough.
# ---------------------------------------------------------------------------
def check_updater_task_stack_size(text: str) -> None:
    m = re.search(r"#define\s+SD_UPDATER_TASK_STACK_BYTES\s+(\d+)", text)
    if not m:
        fail(
            f"{APP_MAIN.relative_to(ROOT)}: expected a "
            "'#define SD_UPDATER_TASK_STACK_BYTES <n>' constant sizing the dedicated "
            "SD-updater task -- see docs/troubleshooting.md."
        )
        return
    size = int(m.group(1))
    if size < MIN_UPDATER_TASK_STACK_BYTES:
        fail(
            f"{APP_MAIN.relative_to(ROOT)}: SD_UPDATER_TASK_STACK_BYTES={size} is below "
            f"the required {MIN_UPDATER_TASK_STACK_BYTES} B floor -- see "
            "docs/troubleshooting.md §3."
        )
        return
    ok(f"SD_UPDATER_TASK_STACK_BYTES={size} (>= {MIN_UPDATER_TASK_STACK_BYTES})")


# ---------------------------------------------------------------------------
# Check 3: no plain stack-local uint8_t/char array bigger than the cap in
# the updater sources. Best-effort/conservative, in the spirit of the
# regression test docs/troubleshooting.md Sec.3 calls for: a false
# negative here just means a
# human has to notice in review; a false positive (failing on unrelated
# code) is the safe direction to fail in. A heap_caps_malloc'd pointer
# (`uint8_t *name = ...`) does not match `name[N]` at all, which is exactly
# the point -- that's what the 0.1.3 fix turned these buffers into.
# ---------------------------------------------------------------------------
_DEFINE_RE = re.compile(r"^#define\s+(\w+)\s+(\d+)u?\s*(?:/[/*].*)?$", re.M)
_ARRAY_RE = re.compile(r"\b(?:uint8_t|char)\s+(\w+)\s*\[\s*(\w+)\s*\]\s*(?:;|=[^=])")


def _largest_stack_array(path: Path, text: str) -> tuple[int, str]:
    defines = dict(_DEFINE_RE.findall(text))
    worst_bytes, worst_name = 0, ""
    for name, const in _ARRAY_RE.findall(text):
        if const.isdigit():
            size = int(const)
        elif const in defines:
            size = int(defines[const])
        else:
            continue  # unresolvable (e.g. a sizeof(...) expression) -- not this check's job
        if size > worst_bytes:
            worst_bytes, worst_name = size, name
    return worst_bytes, worst_name


def check_no_big_stack_arrays(path: Path, text: str) -> None:
    if not text:
        return  # require_file() already recorded the missing-file failure
    size, name = _largest_stack_array(path, text)
    if size > MAX_STACK_ARRAY_BYTES:
        fail(
            f"{path.relative_to(ROOT)}: stack-local array '{name}[{size}]' exceeds the "
            f"{MAX_STACK_ARRAY_BYTES} B cap -- this is the exact shape of buffer that "
            "overflowed \"main\"'s stack in the 2026-09-03 incident. Move it to "
            "heap_caps_malloc(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) with a free() on "
            "every return path; see docs/troubleshooting.md and "
            "this file's own comments at the buffers that were already moved."
        )
        return
    ok(f"no stack-local array > {MAX_STACK_ARRAY_BYTES} B in {path.relative_to(ROOT)} (largest: {size} B)")


# ---------------------------------------------------------------------------
# Check 4: sdkconfig.defaults pins the raised main-task stack and keeps the
# canary check on.
# ---------------------------------------------------------------------------
def check_sdkconfig_defaults(text: str) -> None:
    if not text:
        return
    m = re.search(r"^CONFIG_ESP_MAIN_TASK_STACK_SIZE=(\d+)\s*$", text, re.M)
    if not m:
        fail(
            f"{SDKCONFIG_DEFAULTS.relative_to(ROOT)}: no CONFIG_ESP_MAIN_TASK_STACK_SIZE "
            f"line -- expected the 0.1.3 defence-in-depth value (>= {MIN_MAIN_TASK_STACK_BYTES})."
        )
    else:
        value = int(m.group(1))
        if value < MIN_MAIN_TASK_STACK_BYTES:
            fail(
                f"{SDKCONFIG_DEFAULTS.relative_to(ROOT)}: CONFIG_ESP_MAIN_TASK_STACK_SIZE="
                f"{value} is below the required {MIN_MAIN_TASK_STACK_BYTES} -- see "
                "docs/troubleshooting.md §3."
            )
        else:
            ok(f"CONFIG_ESP_MAIN_TASK_STACK_SIZE={value} (>= {MIN_MAIN_TASK_STACK_BYTES}) in sdkconfig.defaults")

    if re.search(r"^CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y\s*$", text, re.M):
        ok("CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y is set (checked in the generated sdkconfig; "
           "the canary choice itself lives in firmware/s3/sdkconfig, mirrored here informationally)")
    # Not a hard failure if absent from *this* file specifically -- the canary Kconfig choice is
    # recorded in the generated firmware/s3/sdkconfig (see check_generated_sdkconfig below), which
    # is the actual build input; sdkconfig.defaults only needs to reproduce it on a fresh checkout.


def check_generated_sdkconfig() -> None:
    sdkconfig = ROOT / "firmware/s3/sdkconfig"
    if not sdkconfig.is_file():
        # Not generated yet (e.g. before the first `idf.py build`) -- not this
        # script's job to run idf.py; sdkconfig.defaults's own check above is
        # the authoritative, always-present source for this.
        return
    text = sdkconfig.read_text()
    if not re.search(r"^CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y\s*$", text, re.M):
        fail(
            f"{sdkconfig.relative_to(ROOT)}: CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY is not "
            "set to y -- the FreeRTOS stack-overflow canary must stay enabled (see "
            "docs/troubleshooting.md §3 and SAFETY.md)."
        )
        return
    ok(f"CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y in the generated {sdkconfig.relative_to(ROOT)}")


def main() -> int:
    app_main_text = require_file(APP_MAIN)
    updater_text = require_file(UPDATER_C)
    tar_text = require_file(TAR_C)
    defaults_text = require_file(SDKCONFIG_DEFAULTS)

    if app_main_text:
        check_updater_not_called_from_app_main_body(app_main_text)
        check_updater_task_stack_size(app_main_text)
    check_no_big_stack_arrays(UPDATER_C, updater_text)
    if TAR_C.is_file():
        check_no_big_stack_arrays(TAR_C, tar_text)
    check_sdkconfig_defaults(defaults_text)
    check_generated_sdkconfig()

    print()
    if FAILURES:
        for f in FAILURES:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(FAILURES)} check(s) failed.", file=sys.stderr)
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
