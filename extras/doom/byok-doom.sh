#!/usr/bin/env bash
# extras/doom/byok-doom.sh — launch chocolate-doom and mirror it to the BYOK panel.
#
# Usage:
#   extras/doom/byok-doom.sh [OPTIONS] [-- MIRROR_ARGS...]
#
#   --fps N              mirror frame rate to request (default 4, the per-byte-path
#                         ceiling documented in docs/sample-projects/doom.md §5/§6). Passed straight
#                         through to `python -m byok.mirror --fps N`.
#   --bulk                shortcut for `--fps 30` (docs/sample-projects/doom.md §6's bulk-path figure).
#                         Before starting the mirror, tries `byok display --bulk on`
#                         (if that CLI command exists) to flip the device into the
#                         bulk-write I2C path; if it doesn't exist, prints the manual
#                         step instead and continues anyway -- see NOTE below.
#   --invert auto|on|off  passed through to `python -m byok.mirror --invert`.
#                         Default: off -- see CONTRAST below.
#   --dither NAME          passed through to `python -m byok.mirror --dither`
#                         (choices come from render._QUANTIZERS in
#                         host/macos/byok/render.py -- currently none/bayer/floyd).
#                         Default: none -- see CONTRAST below.
#   --mirror-args "STR"    extra arguments appended to the mirror invocation, split
#                         on whitespace (no quoting/escaping -- for anything with
#                         spaces or shell metacharacters, use the `--` form below).
#   -- MIRROR_ARGS...      everything after a literal `--` is appended to the mirror
#                         invocation verbatim, after --mirror-args (if both given).
#                         Since flags are applied left-to-right and argparse keeps
#                         the last value for a repeated flag, anything given here
#                         overrides this launcher's own flags, including
#                         --fit/--band/--full-every. Example:
#                           extras/doom/byok-doom.sh -- --invert on --dither bayer
#
# CONTRAST (owner report 2026-09-04: "the contrast is horrible ... everything is
# too light"). Doom is almost entirely dark by design. The mirror's default
# --invert auto (host/macos/byok/mirror.py's decide_invert(), driven by per-frame
# mean luminance with hysteresis) sees that darkness and inverts it to a light
# background -- exactly backwards for this source, and Bayer dithering on top of
# that inverted image washed out what little contrast remained. This launcher now
# defaults to:
#   --invert off    so the panel shows what the game actually renders (dark
#                    background, bright highlights) instead of an auto-inverted
#                    negative.
#   --dither none    which (paired with the mirror's own default
#                    --threshold-method otsu -- not overridden here) takes the
#                    per-byte-path quantize_none() + otsu_threshold() route
#                    (host/macos/byok/mirror.py ~L779-798): a hard, per-frame
#                    Otsu-optimal black/white cut with no dither noise, instead of
#                    quantize_bayer()'s ordered 4x4 checkerboard of on/off pixels.
#                    Bayer represents Doom's smooth sector-light shading better,
#                    but that same checkerboard reads as "washed out"/lower-
#                    contrast on a 240x80 1-bit panel at normal viewing distance --
#                    the hard Otsu cut is the more legible ("punchy") choice for a
#                    dark game where visibility, not shading fidelity, is the
#                    owner's actual complaint.
# The panel is monochrome 1bpp hardware (docs/display.md) -- there is no --bpp 2
# experiment available on this device; --bpp is left unset here so the mirror
# picks the device's own native_bpp. (--bpp 2 is not offered as a launcher flag
# for this reason -- pass it via `--` only if you specifically want to test it
# against dry-run/preview output, not against the real panel.)
#
# If it still looks wrong after these defaults, there is no numeric quantization-
# threshold flag to hand-tune (mirror.py's own --threshold flag is the dirty-rect
# diff's changed-pixel threshold, NOT the quantization cut point -- see its
# --help). The two things actually worth trying, in order:
#   1st, if still too light or too dark:
#     extras/doom/byok-doom.sh -- --dither bayer
#       (restores ordered dithering -- softer, more graduated, sometimes reads
#       better than a single hard cut depending on the scene.)
#   2nd, if still off:
#     extras/doom/byok-doom.sh -- --threshold-method mean
#       (a cheaper, differently-shaped split point than the default Otsu cut --
#       only affects the --dither none path.)
#   If the image looks like a clean negative of what it should be (dark should be
#   light and vice versa), the fix is polarity, not contrast:
#     extras/doom/byok-doom.sh -- --invert on
#   If it's noisy/grainy rather than too light/dark: --dither bayer trades some
#   of that noise for softer edges; --dither none (the default here) is the
#   less-noisy, harder-edged end of that same trade.
#
# Ctrl-C stops the mirror AND quits chocolate-doom.
#
# Controls (played on the Mac keyboard -- keyboard focus MUST be on the
# chocolate-doom window, not the terminal, for these to reach the game):
#   Arrow keys   move forward/back, turn left/right
#   Ctrl         fire
#   Space        use / open door / flip switch
#   Shift (hold) run
#   1-7          select weapon
#   Tab          automap
#   Esc          menu
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
WAD="$HERE/wad/DOOM1.WAD"
HOST_MACOS="$REPO_ROOT/host/macos"
SMH="$HOST_MACOS/ScreenMirrorHelper/.build/release/ScreenMirrorHelper"
PYTHON="$HOST_MACOS/.venv/bin/python"

FPS=4
BULK=0
INVERT="off"
DITHER="none"
MIRROR_ARGS_STR=""
PASSTHROUGH=()

while [ $# -gt 0 ]; do
  case "$1" in
    --fps)
      FPS="$2"
      shift 2
      ;;
    --bulk)
      BULK=1
      FPS=30
      shift
      ;;
    --invert)
      case "$2" in
        auto|on|off) INVERT="$2" ;;
        *) echo "invalid --invert value: $2 (must be auto, on, or off)" >&2; exit 2 ;;
      esac
      shift 2
      ;;
    --dither)
      DITHER="$2"
      shift 2
      ;;
    --mirror-args)
      MIRROR_ARGS_STR="$2"
      shift 2
      ;;
    --)
      shift
      PASSTHROUGH=("$@")
      break
      ;;
    -h|--help)
      sed -n '2,88p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

# --mirror-args is a plain-whitespace split (no quoting support -- documented in
# the usage header above); `read -ra` does word-splitting only, no globbing, so
# this is safe even if MIRROR_ARGS_STR contains glob metacharacters.
MIRROR_ARGS_EXTRA=()
if [ -n "$MIRROR_ARGS_STR" ]; then
  IFS=' ' read -ra MIRROR_ARGS_EXTRA <<< "$MIRROR_ARGS_STR"
fi

log()  { printf '[byok-doom] %s\n' "$*"; }
fail() { printf '[byok-doom] ERROR: %s\n' "$*" >&2; exit 1; }

[ -f "$WAD" ] || fail "DOOM1.WAD not found at $WAD -- run extras/doom/install.sh first."
command -v chocolate-doom >/dev/null 2>&1 || fail "chocolate-doom not found on PATH -- run extras/doom/install.sh first."
[ -x "$SMH" ] || fail "ScreenMirrorHelper not built at $SMH -- see host/macos/ScreenMirrorHelper/README.md."
[ -x "$PYTHON" ] || fail "host/macos/.venv Python not found at $PYTHON -- set up the venv per host/macos/README first."

# --- 1. device check ---------------------------------------------------
if ! ls /dev/cu.usbmodemBYOKMOD* >/dev/null 2>&1; then
  fail "no BYOK device found (ls /dev/cu.usbmodemBYOKMOD* matched nothing). Plug in the panel and try again."
fi
log "device found: $(ls /dev/cu.usbmodemBYOKMOD* 2>/dev/null | head -1)"

# --- 2. optional bulk-write flip ----------------------------------------
if [ "$BULK" -eq 1 ]; then
  if command -v byok >/dev/null 2>&1 && byok display --bulk on --help >/dev/null 2>&1; then
    log "enabling bulk-write I2C path (byok display --bulk on)..."
    byok display --bulk on
  else
    log "MANUAL STEP: 'byok display --bulk on' does not exist on this install (0.1.14"
    log "  DISPLAY_CFG toggle not shipped yet -- docs/sample-projects/doom.md §2(A)). Flip"
    log "  CONFIG_BYOK_DISPLAY_BULK_WRITES on the device by hand first, or --fps 30"
    log "  will just over-request a per-byte-path panel. Continuing anyway."
  fi
fi

# --- 3. launch chocolate-doom, windowed, backgrounded -------------------
# chocolate-doom's stdout/stderr are captured to a log file (rather than
# left to interleave silently with our own [byok-doom] lines) so that any
# failure -- immediate exit, crash during init, dying mid-wait -- can be
# reported with the actual reason instead of going silent.
DOOM_LOG="${TMPDIR:-/tmp}/byok-doom-chocolate-doom.$$.log"
log "starting chocolate-doom (output logging to $DOOM_LOG)..."
chocolate-doom -iwad "$WAD" -window -geometry 640x400 -nomouse >"$DOOM_LOG" 2>&1 &
DOOM_PID=$!

MIRROR_PID=""
cleanup() {
  log "stopping... (chocolate-doom output logged to $DOOM_LOG)"
  [ -n "$MIRROR_PID" ] && kill "$MIRROR_PID" 2>/dev/null || true
  kill "$DOOM_PID" 2>/dev/null || true
  wait "$DOOM_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# --- 4. wait for its window, then find its ScreenMirrorHelper window id -
# NOTE: the WINDOW_ID lookups below are followed by `|| true`. Under
# `set -euo pipefail`, a pipeline ending in `grep` that matches nothing
# (the expected outcome on early iterations, before the window exists yet)
# exits non-zero and would otherwise kill the whole script via the EXIT
# trap on its very first, silent attempt -- no ERROR text, no explanation.
# `|| true` lets a normal "not found yet" iteration fall through to the
# retry/sleep below instead; the real timeout is still enforced by the
# `[ -n "$WINDOW_ID" ] || fail ...` check after the loop.
WINDOW_WAIT_ATTEMPTS=20
WINDOW_WAIT_SLEEP=0.5
WINDOW_WAIT_HEAD_START=1
# WINDOW_WAIT_TIMEOUT is just the head start plus retries*sleep, folded into
# one number for the log/error text below. If awk is ever unavailable or
# misbehaves, fall back to "?" rather than letting a bare command
# substitution assignment fail under `set -euo pipefail` and kill the script.
WINDOW_WAIT_TIMEOUT="$(awk -v a="$WINDOW_WAIT_ATTEMPTS" -v s="$WINDOW_WAIT_SLEEP" -v h="$WINDOW_WAIT_HEAD_START" 'BEGIN { printf "%.1f", h + a * s }')" || WINDOW_WAIT_TIMEOUT="?"
log "waiting for the Doom window (up to ~${WINDOW_WAIT_TIMEOUT}s: ${WINDOW_WAIT_HEAD_START}s head start + ${WINDOW_WAIT_ATTEMPTS}x ${WINDOW_WAIT_SLEEP}s retries)..."
# Give chocolate-doom a head start through zone/WAD/SDL init before the
# first lookup attempt -- without this, the first attempt fires at
# essentially t=0, before the window can plausibly exist yet.
sleep "$WINDOW_WAIT_HEAD_START"
WINDOW_ID=""
for _ in $(seq 1 "$WINDOW_WAIT_ATTEMPTS"); do
  if ! kill -0 "$DOOM_PID" 2>/dev/null; then
    DOOM_EXIT=0
    wait "$DOOM_PID" 2>/dev/null || DOOM_EXIT=$?
    fail "chocolate-doom exited before its window appeared (exit code $DOOM_EXIT). Full log: $DOOM_LOG
Last output:
$(tail -n 20 "$DOOM_LOG" 2>/dev/null)"
  fi
  WINDOW_ID=$("$SMH" list 2>/dev/null | grep -i 'app="chocolate-doom"' | head -1 | grep -oE 'id=[0-9]+' | head -1 | cut -d= -f2) || true
  if [ -z "$WINDOW_ID" ]; then
    # fall back to matching on title in case the app= field differs
    WINDOW_ID=$("$SMH" list 2>/dev/null | grep -i 'doom' | grep -vi 'byok\|code' | head -1 | grep -oE 'id=[0-9]+' | head -1 | cut -d= -f2) || true
  fi
  [ -n "$WINDOW_ID" ] && break
  sleep "$WINDOW_WAIT_SLEEP"
done
[ -n "$WINDOW_ID" ] || fail "couldn't find the Doom window via '$SMH list' -- timed out after ~${WINDOW_WAIT_TIMEOUT}s. chocolate-doom has been stopped. Full log: $DOOM_LOG"
log "found Doom window id=$WINDOW_ID"

# --- 5. start the mirror (docs/sample-projects/doom.md §5 recommended flags, contrast
# defaults per the CONTRAST note in the usage header above) --------------
log "starting mirror at --fps $FPS --invert $INVERT --dither $DITHER (Ctrl-C to stop both)..."
cd "$HOST_MACOS"
"$PYTHON" -m byok.mirror --window "$WINDOW_ID" \
  --fit band --band middle \
  --dither "$DITHER" \
  --invert "$INVERT" \
  --fps "$FPS" --full-every 1 \
  "${MIRROR_ARGS_EXTRA[@]+"${MIRROR_ARGS_EXTRA[@]}"}" \
  "${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}" &
MIRROR_PID=$!
wait "$MIRROR_PID"
