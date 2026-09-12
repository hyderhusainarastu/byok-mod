"""`byok.mirror` — Mode 2 (screen mirror) pipeline, standalone module.

Usage::

    python -m byok.mirror --list
    python -m byok.mirror --display 1 --fps 2
    python -m byok.mirror --window 1234 --dither bayer --full-every 30
    python -m byok.mirror --display 1 --dry-run --preview /tmp/mirror-preview
    python -m byok.mirror --window 1234 --scale 3 --fps 2   # text window (--fit fill,
                                                              # --dither none, --invert auto
                                                              # are already the defaults
                                                              # for --window; see the
                                                              # mirror sample-project doc §4)

This module is deliberately independent of `cli.py` (owned by a separate
development track -- see docs/sample-projects/mirror-and-virtual-display.md §1
and the project's
SAFETY.md for why): it is runnable on its own as `python -m byok.mirror`, wires up its
own argparse, and does not import or modify `cli.py`. Whoever later adds a
`byok mirror` subcommand can simply call `mirror.main()` from `cmd_mirror`.

Pipeline (docs/sample-projects/mirror-and-virtual-display.md §4, now implemented):

  1. Spawn `ScreenMirrorHelper capture` (building it with `swift build -c
     release` first if the binary is missing) with `--width`/`--height` set
     to the *target panel size* (240x80, or whatever a connected device's
     own HELLO_ACK reports -- never hard-coded when a device is attached).
  2. Read one `SMH1` binary frame record at a time from its stdout (wire
     format: `ScreenMirrorHelper/README.md`, docs/sample-projects/mirror-and-virtual-display.md §3).
  3. Turn each frame's raw 8-bit grayscale canvas into a PIL `Image`,
     optionally cropped to `--region` and re-letterboxed back to the full
     target size (a Python-side "zoom into a rectangle" -- the helper
     itself has no crop flag, by design; see docs/host-tools.md §1's
     sub-mode 2b).
  4. Quantize with `byok.render.quantize_levels` (bayer/floyd/none,
     imported, not reimplemented) to the device's bit depth.
  5. Diff against the previous *quantized* frame with
     `byok.render.choose_dirty_plan` (imported) -- deliberately re-diffing
     post-dither rather than trusting the helper's own pre-dither dirty
     rect, exactly because dithering both hides and invents apparent
     changes (docs/sample-projects/mirror-and-virtual-display.md §4).
  6. Send via `byok.device.Device`: first frame always full
     (FRAME_BEGIN/FRAME_DATA/FRAME_END + FULL_REFRESH), every `--full-every`
     frames after that also full, everything else as one `DRAW_BITMAP` +
     `PARTIAL_REFRESH` per changed page-band (or the FRAME_* partial-stream
     fallback for a rect too big for one DRAW_BITMAP) -- the same wire
     strategy `byok.dashboard.loop.DashboardLoop` already uses; a
     `TransportError` mid-cycle reconnects with backoff and forces the next
     send to be full, also matching that module.

`--dry-run` runs the exact same planning/quantizing code path with no
`Device` at all: nothing is sent, per-frame stats (dirty bytes, estimated
I2C transactions) are printed, and `--preview PATH` (treated as a
directory in dry-run) gets one numbered PNG per frame for eyeballing the
pipeline without hardware.
"""

from __future__ import annotations

import argparse
import logging
import os
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Callable, List, Optional, Sequence, Tuple

from PIL import Image, ImageOps

from . import proto, render
from .device import Device, FULL_REFRESH_MODE, NackReceived, PARTIAL_REFRESH_MODE
from .transport import HandshakeFailed, NoDeviceFound, TransportError, WrongDevice

logger = logging.getLogger("byok.mirror")

DEFAULT_WIDTH = 240
DEFAULT_HEIGHT = 80
DEFAULT_FPS = 2.0
MIN_FPS = 0.1
# Raised from 4.0 to 30.0 (extras/doom/, docs/sample-projects/doom.md §5) to allow --fps up to
# the bulk-write I2C path's theoretical ceiling (~29ms/refresh, 0.1.0's own
# measurement, docs/sample-projects/doom.md §2(A)). NOTE (--raw-fps): raising this constant
# does NOT itself change what the panel can sustain -- the per-byte path
# shipping today (CONFIG_BYOK_DISPLAY_BULK_WRITES=n) is still ~220ms/full
# refresh, so any --fps above ~4-5 will request frames faster than the
# hardware can draw them and just queue/drop, not actually run "raw" at that
# rate. Only once the 0.1.14 DISPLAY_CFG bulk-write toggle exists, is flipped
# on, and has been re-verified on the glass (docs/sample-projects/doom.md §2(A)) does a
# higher --fps reflect real achievable throughput rather than a raw request
# rate the panel can't keep up with.
MAX_FPS = 30.0  # ~220ms full refresh (per-byte, today) / ~29ms (bulk, pending re-verification) -- docs/sample-projects/doom.md §5
DEFAULT_FULL_EVERY = 60
DEFAULT_THRESHOLD = 8
DEFAULT_PAGE_ROWS = 8  # matches byok.dashboard.loop's partial-refresh page granularity

_RESAMPLE = Image.LANCZOS if hasattr(Image, "LANCZOS") else Image.BICUBIC

# --fit auto-invert hysteresis band (mean 8-bit luminance, 0=black..255=white):
# once inverted, luminance must climb back above INVERT_OFF_ABOVE before we
# un-invert; once normal, it must drop below INVERT_ON_BELOW before we
# invert. A mean sitting between the two keeps whatever state was already
# in effect, so a frame hovering near "medium gray" doesn't flip every
# cycle (2026-09-03 on-device note: a dark TextEdit background rendered as
# a dither mesh with no inversion at all -- see docs/host-tools.md §11).
INVERT_ON_BELOW = 96.0
INVERT_OFF_ABOVE = 150.0

# DRAW_BITMAP's header is 10 B (docs/protocol.md §6.2); a packed rect at or
# under this many bytes fits one DRAW_BITMAP frame -- same constant
# byok.dashboard.loop uses, reproduced here since this module must not
# import dashboard/loop.py (maintained separately).
_MAX_DRAW_BITMAP_PIXELS = proto.MAX_PAYLOAD - 10

_SMH1_MAGIC = b"SMH1"
_SMH1_HEADER_STRUCT = ">4sHHHHHH"  # magic,width,height,dirty_x,dirty_y,dirty_w,dirty_h
_SMH1_HEADER_LEN = struct.calcsize(_SMH1_HEADER_STRUCT)

_PERMISSION_MSG = (
    "ScreenMirrorHelper: Screen Recording permission is not granted.\n"
    "Grant it in System Settings > Privacy & Security > Screen Recording for the\n"
    "terminal app you are running `python -m byok.mirror` from (Terminal, iTerm,\n"
    "VS Code's terminal, etc.), then re-run the command. This is a one-time grant\n"
    "per app; ScreenMirrorHelper never prompts for it itself."
)


def _indent_stderr(text: str) -> str:
    """Format a (possibly multi-line, possibly empty) helper stderr capture
    for display under an error message: every line indented and prefixed so
    it's visually distinct from the message it's attached to, and nothing
    is dropped -- a bare signal kill (SIGTRAP/SIGABRT) may produce zero
    lines, one line (a single C-level assertion), or several (with
    --debug), and all of them should show up here."""
    if not text:
        return ""
    return "\n".join(f"    | {line}" for line in text.splitlines())


class MirrorError(Exception):
    """Base for this module's own errors (distinct from transport/device errors)."""


class HelperError(MirrorError):
    """The Swift capture helper could not be built, started, or understood."""


class HelperPermissionError(HelperError):
    """The helper exited 3: Screen Recording permission is not granted."""


class HelperExited(HelperError):
    """The helper's stdout closed (EOF) -- process exited or was killed."""


class _HelperEOF(Exception):
    """Internal signal: `_read_frame` hit EOF. Never escapes `run_once`."""


# --------------------------------------------------------------------------
# Locating / building the Swift helper
# --------------------------------------------------------------------------


def default_helper_dir() -> str:
    return os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ScreenMirrorHelper")
    )


def helper_binary_path(helper_dir: Optional[str] = None) -> str:
    return os.path.join(helper_dir or default_helper_dir(), ".build", "release", "ScreenMirrorHelper")


def ensure_helper_built(helper_dir: Optional[str] = None) -> str:
    """Return the built helper binary's path, running `swift build -c
    release` in `helper_dir` first if it is missing."""
    helper_dir = helper_dir or default_helper_dir()
    binary = helper_binary_path(helper_dir)
    if os.path.isfile(binary):
        return binary
    logger.info("ScreenMirrorHelper binary not found at %s; building (swift build -c release)...", binary)
    try:
        subprocess.run(["swift", "build", "-c", "release"], cwd=helper_dir, check=True)
    except FileNotFoundError as exc:
        raise HelperError(
            "no `swift` toolchain on PATH -- install the Xcode Command Line Tools, "
            f"or build manually: cd {helper_dir!r} && swift build -c release"
        ) from exc
    except subprocess.CalledProcessError as exc:
        raise HelperError(
            f"`swift build -c release` failed (exit {exc.returncode}) in {helper_dir!r}"
        ) from exc
    if not os.path.isfile(binary):
        raise HelperError(f"swift build succeeded but {binary!r} was not produced")
    return binary


def spawn_capture(
    argv_prefix: List[str],
    *,
    display: Optional[int] = None,
    window: Optional[int] = None,
    width: int,
    height: int,
    fps: float,
    threshold: int = DEFAULT_THRESHOLD,
) -> "subprocess.Popen":
    """Start `<argv_prefix> capture ...`. `argv_prefix` is the helper
    executable path as a one-element list in normal use; tests pass e.g.
    `[sys.executable, fake_helper_script]` instead, so this function never
    hard-codes anything about the real Swift binary beyond the CLI it
    documents (`ScreenMirrorHelper/README.md` /
    docs/sample-projects/mirror-and-virtual-display.md §3)."""
    if (display is None) == (window is None):
        raise ValueError("spawn_capture needs exactly one of display= or window=")
    args = list(argv_prefix) + ["capture"]
    args += ["--display", str(display)] if display is not None else ["--window", str(window)]
    args += [
        "--width", str(int(width)),
        "--height", str(int(height)),
        "--fps", str(max(1, round(fps))),
        "--threshold", str(int(threshold)),
    ]
    try:
        return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as exc:
        raise HelperError(f"failed to start ScreenMirrorHelper ({' '.join(args)}): {exc}") from exc


def run_list(argv_prefix: List[str]) -> str:
    """Run `<argv_prefix> list` and return its stdout. Raises
    HelperPermissionError / HelperError on a non-zero exit."""
    try:
        proc = subprocess.run(list(argv_prefix) + ["list"], capture_output=True)
    except OSError as exc:
        raise HelperError(f"failed to run ScreenMirrorHelper list: {exc}") from exc
    if proc.returncode == 3:
        stderr = proc.stderr.decode("utf-8", "replace").strip()
        raise HelperPermissionError(stderr or _PERMISSION_MSG)
    if proc.returncode != 0:
        stderr = proc.stderr.decode("utf-8", "replace").strip()
        raise HelperError(
            f"ScreenMirrorHelper list failed (exit {proc.returncode})"
            + (f":\n{_indent_stderr(stderr)}" if stderr else "")
        )
    return proc.stdout.decode("utf-8", "replace")


def terminate_process(proc: Optional["subprocess.Popen"]) -> None:
    if proc is None:
        return
    if proc.poll() is None:
        try:
            proc.terminate()
            proc.wait(timeout=2.0)
        except Exception:  # noqa: BLE001 - best-effort teardown
            try:
                proc.kill()
            except Exception:  # noqa: BLE001
                pass
    # Close the pipes even if the process had already exited on its own
    # (e.g. a finite-frame test helper) -- Popen does not do this for you,
    # and leaving them open trips ResourceWarning under -W error / pytest.
    for pipe in (proc.stdout, proc.stderr, proc.stdin):
        if pipe is not None:
            try:
                pipe.close()
            except Exception:  # noqa: BLE001
                pass


# --------------------------------------------------------------------------
# SMH1 frame reading
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class MirrorFrame:
    width: int
    height: int
    dirty_x: int
    dirty_y: int
    dirty_w: int
    dirty_h: int
    pixels: bytes  # 8-bit grayscale, row-major, width*height bytes


def read_exact(stream, n: int) -> Optional[bytes]:
    """Read exactly `n` bytes from `stream` (any object with `.read(k)`).
    Returns None on a clean EOF with zero bytes consumed; loops across
    however many short reads it takes otherwise (a subprocess pipe, or a
    test double, may hand back fewer bytes than asked for per call)."""
    if n <= 0:
        return b""
    chunks: List[bytes] = []
    remaining = n
    while remaining > 0:
        chunk = stream.read(remaining)
        if not chunk:
            return None if not chunks else b"".join(chunks)  # truncated mid-read
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_smh1_frame(stream) -> Optional[MirrorFrame]:
    """Read one SMH1 record from `stream`. Returns None on a clean EOF
    before any header bytes arrive (the ordinary "helper has exited" case).
    Raises HelperError on a bad magic or a frame truncated partway through
    the header or the pixel payload."""
    header = read_exact(stream, _SMH1_HEADER_LEN)
    if header is None:
        return None
    if len(header) != _SMH1_HEADER_LEN:
        raise HelperError(f"SMH1 stream truncated in header ({len(header)}/{_SMH1_HEADER_LEN} bytes)")
    magic, width, height, dirty_x, dirty_y, dirty_w, dirty_h = struct.unpack(_SMH1_HEADER_STRUCT, header)
    if magic != _SMH1_MAGIC:
        raise HelperError(f"bad SMH1 magic {magic!r} (helper stdout is not framed as expected)")
    npixels = width * height
    pixels = read_exact(stream, npixels)
    if pixels is None or len(pixels) != npixels:
        got = 0 if pixels is None else len(pixels)
        raise HelperError(f"SMH1 frame truncated: got {got}/{npixels} pixel bytes")
    return MirrorFrame(width, height, dirty_x, dirty_y, dirty_w, dirty_h, pixels)


# --------------------------------------------------------------------------
# fps rate limiting
# --------------------------------------------------------------------------


class RateLimiter:
    """Blocks `wait()` just long enough that consecutive calls are spaced
    at least `1/fps` apart. `clock`/`sleep` are injectable so tests never
    pay real wall-clock time (see `_fake_transport.ManualClock`)."""

    def __init__(
        self,
        fps: float,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
    ):
        self.min_interval = (1.0 / fps) if fps and fps > 0 else 0.0
        self.clock = clock
        self.sleep = sleep
        self._last: Optional[float] = None

    def wait(self) -> None:
        if self.min_interval <= 0:
            return
        now = self.clock()
        if self._last is not None:
            remaining = self.min_interval - (now - self._last)
            if remaining > 0:
                self.sleep(remaining)
                now = self.clock()
        self._last = now


# --------------------------------------------------------------------------
# --fit geometry, auto-threshold, auto-invert (all pure functions, no PIL
# dependency beyond the plain pixel sequences/tuples callers already have --
# kept this way so they're each independently unit-testable)
# --------------------------------------------------------------------------


def compute_band_rect(
    frame_w: int, frame_h: int, target_w: int, target_h: int, band: str = "middle"
) -> Tuple[int, int, int, int]:
    """The largest `target_w:target_h`-aspect rectangle that fits inside a
    `frame_w`x`frame_h` capture -- a "crop to cover" (as opposed to
    `render.fit_to_display`'s "crop to contain") selection. This is what
    `--fit fill`/`--fit band` crop out of an (ideally larger-than-panel,
    see `--scale` and the capture-size heuristic in `main()`) capture
    before scaling the crop down to the panel size, so the result fills
    the panel edge-to-edge with real content instead of `fit_to_display`'s
    letterbox bars.

    Exactly one axis is cropped -- whichever one makes the source
    "relatively wider" or "relatively taller" than the target aspect
    ratio -- and the other axis is used in full. `band` ("top"/"middle"/
    "bottom") only has an effect when the *height* is the cropped axis
    (the common case: a roughly square or portrait source cropped down to
    a wide panel); the cropped width case is always centered, since there
    is no equivalent "top/bottom" concept along that axis. `band` is
    otherwise ignored -- "middle" (the default) is what `--fit fill`
    always uses.
    """
    if frame_w <= 0 or frame_h <= 0 or target_w <= 0 or target_h <= 0:
        return (0, 0, max(1, frame_w), max(1, frame_h))
    target_ar = target_w / target_h
    frame_ar = frame_w / frame_h
    if frame_ar > target_ar:
        # Source is relatively wider than the target -- crop width, keep
        # the full height, centered horizontally.
        crop_h = frame_h
        crop_w = max(1, min(frame_w, round(frame_h * target_ar)))
        x = (frame_w - crop_w) // 2
        y = 0
    else:
        # Source is relatively taller than (or exactly as wide as) the
        # target -- crop height, keep the full width. This is the case
        # `--band` picks a vertical position for.
        crop_w = frame_w
        crop_h = max(1, min(frame_h, round(frame_w / target_ar)))
        x = 0
        if band == "top":
            y = 0
        elif band == "bottom":
            y = frame_h - crop_h
        else:  # "middle", also the fallback for an unrecognized value
            y = (frame_h - crop_h) // 2
    return (x, y, crop_w, crop_h)


def otsu_threshold(pixels: Sequence[int]) -> int:
    """Otsu's method: the 0..255 grayscale split point that maximizes the
    between-class variance of the pixel histogram -- i.e. the cut that
    best separates two populations (e.g. "text" and "background") without
    assuming they straddle a fixed midpoint like plain `render.quantize_none`
    does. Falls back to 128 for an empty image or one with no variance to
    maximize (uniform luminance -- any threshold is equally (un)informative)."""
    hist = [0] * 256
    for v in pixels:
        hist[v] += 1
    total = len(pixels)
    if total == 0:
        return 128
    sum_total = sum(i * count for i, count in enumerate(hist))
    sum_b = 0.0
    weight_b = 0
    best_var = -1.0
    best_t = 128
    for t in range(256):
        weight_b += hist[t]
        if weight_b == 0:
            continue
        weight_f = total - weight_b
        if weight_f == 0:
            break
        sum_b += t * hist[t]
        mean_b = sum_b / weight_b
        mean_f = (sum_total - sum_b) / weight_f
        var_between = weight_b * weight_f * (mean_b - mean_f) ** 2
        if var_between > best_var:
            best_var = var_between
            best_t = t
    return best_t


def mean_threshold(pixels: Sequence[int]) -> int:
    """Cheaper (no histogram pass beyond a sum) alternative to
    `otsu_threshold`: just the mean luminance, rounded. A reasonable
    fallback when Otsu's implicit bimodal-histogram assumption doesn't
    hold for a given source."""
    if not pixels:
        return 128
    return round(sum(pixels) / len(pixels))


def mean_luminance(pixels: Sequence[int]) -> float:
    if not pixels:
        return 0.0
    return sum(pixels) / len(pixels)


def decide_invert(luminance: float, previous: bool, mode: str = "auto") -> bool:
    """Whether to invert this frame's canvas before quantizing. `mode`
    "on"/"off" are unconditional; "auto" applies `INVERT_ON_BELOW`/
    `INVERT_OFF_ABOVE` hysteresis around `luminance` (mean 0..255) so a
    value in the dead zone between the two thresholds keeps `previous`'s
    state rather than flip-flopping frame to frame."""
    if mode == "on":
        return True
    if mode == "off":
        return False
    if luminance < INVERT_ON_BELOW:
        return True
    if luminance > INVERT_OFF_ABOVE:
        return False
    return previous


# --------------------------------------------------------------------------
# The pipeline
# --------------------------------------------------------------------------


@dataclass
class MirrorCycleStats:
    cycle: int
    full: bool
    rects: int
    dirty_bytes: int
    est_i2c_txns: int
    reconnected: bool = False
    mean_luminance: float = 0.0
    invert: bool = False
    threshold: Optional[int] = None


class MirrorPipeline:
    """Owns one capture session end to end: reads `SMH1` frames from
    `stream`, quantizes/diffs/sends them. `device=None` is dry-run mode --
    every other step (including planning what *would* be sent) still runs,
    nothing is transmitted, and `--dry-run`'s per-frame stats are printed.

    **No `byok.notify_ipc` loop lock here** (unlike
    `dashboard.loop.DashboardLoop.run`, which holds one for
    `cmd_notify`'s IPC-first check) -- deliberate, not an oversight: `byok
    mirror` (`cli.py cmd_mirror`) is still a stub ("not yet implemented"),
    so nothing in this tree currently starts a `MirrorPipeline` against a
    real device outside tests; adding filesystem-lock side effects (and
    the constructor param / test churn that comes with it) to an unwired
    path isn't practical yet. `extras/doom/byok-doom.sh` is the one real
    caller of `python -m byok.mirror` today, and nothing else in this
    tree currently races it for the port. Revisit adding a lock here
    if/when `cmd_mirror` is wired up for real use, or if another caller
    of `python -m byok.mirror` is added."""

    def __init__(
        self,
        *,
        device: Optional[Device],
        stream,
        width: int,
        height: int,
        bpp: int,
        dither: str = "bayer",
        region: Optional[Tuple[int, int, int, int]] = None,
        fit: str = "contain",
        band: str = "middle",
        invert: str = "auto",
        threshold_method: str = "otsu",
        fps: float = DEFAULT_FPS,
        full_every: int = DEFAULT_FULL_EVERY,
        preview_path: Optional[str] = None,
        dry_run: bool = False,
        page_rows: int = DEFAULT_PAGE_ROWS,
        reconnect_max_attempts: Optional[int] = None,
        process: Optional["subprocess.Popen"] = None,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
        print_stats: bool = True,
        log: Optional[logging.Logger] = None,
    ):
        if dither not in render._QUANTIZERS:
            raise ValueError(f"unknown dither method {dither!r}; choose from {sorted(render._QUANTIZERS)}")
        if bpp not in (1, 2):
            raise ValueError(f"bpp must be 1 or 2, got {bpp}")
        if device is not None and dry_run:
            raise ValueError("dry_run=True must not be combined with a real device")
        if fit not in ("contain", "fill", "stretch", "band"):
            raise ValueError(f"unknown fit mode {fit!r}; choose from contain, fill, stretch, band")
        if band not in ("top", "middle", "bottom"):
            raise ValueError(f"unknown band {band!r}; choose from top, middle, bottom")
        if invert not in ("auto", "on", "off"):
            raise ValueError(f"unknown invert mode {invert!r}; choose from auto, on, off")
        if threshold_method not in ("otsu", "mean", "fixed"):
            raise ValueError(
                f"unknown threshold method {threshold_method!r}; choose from otsu, mean, fixed"
            )

        self.device = device
        self.stream = stream
        self.width = width
        self.height = height
        self.bpp = bpp
        self.dither = dither
        self.region = region
        self.fit = fit
        self.band = band
        self.invert = invert
        self.threshold_method = threshold_method
        self.full_every = full_every
        self.preview_path = preview_path
        self.dry_run = dry_run
        self.page_rows = page_rows
        self.reconnect_max_attempts = reconnect_max_attempts
        self.process = process
        self.print_stats = print_stats
        self.log = log or logger

        self._rate_limiter = RateLimiter(fps, clock=clock, sleep=sleep)
        self._prev_levels: Optional[render.Levels] = None
        self._cycle = 0
        self._invert_state = False

    # -- frame -> canvas image --------------------------------------------

    def _canvas_image(self, frame: MirrorFrame) -> Image.Image:
        img = Image.frombytes("L", (frame.width, frame.height), frame.pixels)

        if self.region is not None:
            # An explicit --region always wins over --fit's own band/crop
            # selection -- the user picked exact capture-pixel coordinates.
            rx, ry, rw, rh = self.region
            rx = max(0, min(frame.width - 1, rx))
            ry = max(0, min(frame.height - 1, ry))
            rw = max(1, min(frame.width - rx, rw))
            rh = max(1, min(frame.height - ry, rh))
            cropped = img.crop((rx, ry, rx + rw, ry + rh))
            if self.fit == "stretch":
                return cropped.resize((self.width, self.height), resample=_RESAMPLE)
            return render.fit_to_display(cropped, self.width, self.height, bg=0)

        if self.fit == "stretch":
            # Ignore aspect ratio entirely -- resize whatever came back
            # straight onto the panel size, distorting it if the capture's
            # own aspect ratio doesn't match.
            return img.resize((self.width, self.height), resample=_RESAMPLE)

        if self.fit in ("fill", "band"):
            # "Crop to cover": pull the panel-aspect-ratio strip that best
            # covers the capture (see compute_band_rect's docstring) and
            # scale *that* to the panel size, instead of letterboxing the
            # whole capture. `main()` requests a capture noticeably larger
            # than the panel for these two modes specifically so this crop
            # has real content to work with rather than mostly letterbox
            # bars from the helper's own scale-to-fit -- see its capture
            # sizing comment and docs/sample-projects/mirror-and-virtual-display.md §4.
            bx, by, bw, bh = compute_band_rect(frame.width, frame.height, self.width, self.height, self.band)
            cropped = img.crop((bx, by, bx + bw, by + bh))
            # The crop is already exactly the panel's aspect ratio, so this
            # is a plain proportional (LANCZOS) resize, never a letterbox.
            return render.fit_to_display(cropped, self.width, self.height, bg=0)

        # --fit contain (default): the helper already scale-to-fits +
        # letterboxes into whatever width/height it was spawned with, so
        # this is normally a same-size no-op -- except under --scale N
        # (N>1), where the helper was deliberately spawned larger than the
        # panel for supersampling, and this LANCZOS-downscales the whole
        # (already letterboxed) canvas back down, same as a plain
        # fit_to_display would for a source whose aspect ratio already
        # matches the target (which it always does here, since capture
        # size is just the panel size times a scalar).
        if (frame.width, frame.height) != (self.width, self.height):
            self.log.debug(
                "helper frame is %dx%d, panel is %dx%d -- re-fitting "
                "(expected under --scale >1; a defensive fallback otherwise)",
                frame.width, frame.height, self.width, self.height,
            )
            return render.fit_to_display(img, self.width, self.height, bg=0)
        return img

    @staticmethod
    def _levels_to_preview_image(levels: render.Levels, w: int, h: int, bpp: int) -> Image.Image:
        max_level = (2 ** bpp) - 1
        step = 255.0 / max_level
        data = bytes(max(0, min(255, round(v * step))) for v in levels)
        return Image.frombytes("L", (w, h), data)

    def _write_preview(self, levels: render.Levels, w: int, h: int) -> None:
        img = self._levels_to_preview_image(levels, w, h, self.bpp)
        if self.dry_run:
            os.makedirs(self.preview_path, exist_ok=True)
            img.save(os.path.join(self.preview_path, f"frame-{self._cycle:06d}.png"))
        else:
            img.save(self.preview_path)

    # -- planning (shared by dry-run and live sending) ---------------------

    def _plan(
        self, prev: Optional[render.Levels], curr: render.Levels, w: int, h: int, force_full: bool
    ) -> Tuple[List[render.DirtyRect], bool]:
        if force_full:
            return [render.DirtyRect(0, 0, w, h)], True
        rects, _is_bounding = render.choose_dirty_plan(prev, curr, w, h, self.bpp, page=self.page_rows)
        return rects, False

    @staticmethod
    def _plan_cost(rects: List[render.DirtyRect], bpp: int) -> Tuple[int, int]:
        """(dirty_bytes, est_i2c_txns) for `rects`."""
        dirty_bytes = sum(render.stride_for(r.w, bpp) * r.h for r in rects)
        est_txns = sum(render.estimate_i2c_transactions(r, bpp) for r in rects)
        return dirty_bytes, est_txns

    # -- sending ------------------------------------------------------------

    def _send_rects(self, rects: List[render.DirtyRect], levels: render.Levels, w: int, h: int, full: bool) -> None:
        if full:
            packed = render.pack_framebuffer(levels, w, h, self.bpp)
            self.device._send_frame(w, h, self.bpp, packed, refresh=FULL_REFRESH_MODE)
            return
        for rect in rects:
            cropped = render.crop_levels(levels, w, rect)
            packed = render.pack_framebuffer(cropped, rect.w, rect.h, self.bpp)
            if len(packed) <= _MAX_DRAW_BITMAP_PIXELS:
                self.device.draw_bitmap(rect.x, rect.y, rect.w, rect.h, self.bpp, packed, op=0)
                self.device.refresh(rect=(rect.x, rect.y, rect.w, rect.h))
            else:
                self.device._send_frame(
                    rect.w, rect.h, self.bpp, packed,
                    refresh=PARTIAL_REFRESH_MODE, origin=(rect.x, rect.y),
                )

    def _reconnect(self) -> None:
        transport = self.device.transport
        try:
            transport.close()
        except Exception:  # noqa: BLE001 - best-effort before reconnecting
            self.log.debug("error closing transport before reconnect", exc_info=True)
        transport.connect_with_backoff(do_hello=True, max_attempts=self.reconnect_max_attempts)
        new_w, new_h = self.device.display_size
        if (new_w, new_h) != (self.width, self.height):
            self.log.warning(
                "device geometry changed after reconnect (%dx%d -> %dx%d); "
                "capture is still %dx%d until the process is restarted",
                self.width, self.height, new_w, new_h, self.width, self.height,
            )
        self._prev_levels = None

    # -- one cycle ------------------------------------------------------

    def _read_frame(self) -> MirrorFrame:
        frame = read_smh1_frame(self.stream)
        if frame is None:
            raise _HelperEOF()
        return frame

    def _handle_helper_eof(self) -> None:
        """The SMH1 stream hit EOF. Waits for the helper process to
        actually exit and either returns -- a clean, expected end of
        stream (exit code 0; the real helper never exits on its own once
        capturing, so this only happens if something outside this module
        stopped it deliberately) -- or raises `HelperPermissionError`
        (exit 3) / `HelperExited` (anything else, including "didn't exit
        within the wait budget")."""
        rc: Optional[int] = None
        if self.process is not None:
            try:
                rc = self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                rc = None
        # Read to EOF, not just whatever the first read() call happens to
        # return -- the process has already exited (rc is set above, or
        # we gave up waiting), so its stderr fd is either closed or about
        # to be, and a single .read() with no size argument blocks until
        # EOF and returns everything, however many lines that is. What
        # used to look like "only the first line" in practice was usually
        # just that the crash (a bare SIGTRAP/SIGABRT) produced only one
        # line of libdispatch/CoreGraphics assertion text to begin with --
        # this still surfaces every line that's actually there, and now
        # logs (rather than silently drops) a failure to read them.
        stderr_text = ""
        if self.process is not None and self.process.stderr is not None:
            try:
                stderr_text = self.process.stderr.read().decode("utf-8", "replace").strip()
            except Exception:  # noqa: BLE001
                self.log.debug("failed to read ScreenMirrorHelper stderr after exit", exc_info=True)
        stderr_suffix = ("\n" + _indent_stderr(stderr_text)) if stderr_text else ""
        if rc == 3:
            raise HelperPermissionError(stderr_text or _PERMISSION_MSG)
        if rc == 0:
            self.log.info("ScreenMirrorHelper closed its output stream cleanly (exit 0)")
            return
        if rc is None:
            raise HelperExited(
                "ScreenMirrorHelper closed its output stream but did not exit within 2s"
                + stderr_suffix
            )
        raise HelperExited(f"ScreenMirrorHelper exited with code {rc}" + stderr_suffix)

    def run_once(self) -> MirrorCycleStats:
        frame = self._read_frame()
        canvas = self._canvas_image(frame)

        pixels = list(canvas.getdata())
        luminance = mean_luminance(pixels)
        invert_now = decide_invert(luminance, self._invert_state, self.invert)
        self._invert_state = invert_now
        if invert_now:
            canvas = ImageOps.invert(canvas)
            pixels = list(canvas.getdata())

        threshold_value: Optional[int] = None
        if (
            self.dither == "none"
            and self.bpp == 1
            and self.fit in ("fill", "band")
            and self.threshold_method != "fixed"
        ):
            # Plain `render.quantize_none` splits at a fixed 127.5 midpoint,
            # which is the wrong call for real captured text/background
            # contrast that rarely straddles gray exactly in the middle --
            # compute the cut point from this frame's own histogram instead
            # (docs/sample-projects/mirror-and-virtual-display.md §4).
            threshold_value = (
                mean_threshold(pixels) if self.threshold_method == "mean" else otsu_threshold(pixels)
            )
            # `<=`, not `<`: otsu_threshold returns the histogram bin that
            # belongs to the *low* (dark) class -- for a clean bimodal
            # histogram (e.g. pure black/white text) that bin is often 0
            # itself (every tie is broken toward the smallest t), and `<`
            # would then misclassify every pixel, 0s included, as light.
            levels = [0 if p <= threshold_value else 1 for p in pixels]
            w, h = canvas.size
        else:
            levels, w, h = render.quantize_levels(canvas, levels=2 ** self.bpp, method=self.dither)

        force_full = self._prev_levels is None or (
            self.full_every > 0 and self._cycle % self.full_every == 0
        )

        self._rate_limiter.wait()

        rects, full = self._plan(self._prev_levels, levels, w, h, force_full)
        dirty_bytes, est_txns = self._plan_cost(rects, self.bpp)
        reconnected = False

        if not self.dry_run:
            try:
                self._send_rects(rects, levels, w, h, full)
            except (TransportError, NackReceived) as exc:
                # NackReceived alongside TransportError for the same reason
                # dashboard/loop.py's run_once() catches both -- see that
                # module's comment. transport.request() already absorbs
                # the informational NACK/E_SEQ_GAP itself (its own
                # docstring), so a NackReceived reaching here is a genuine
                # protocol-level refusal; a reconnect (fresh HELLO) is the
                # same recovery a dropped link gets.
                self.log.warning("cycle %d: link error (%s); reconnecting", self._cycle, exc)
                self._reconnect()
                reconnected = True
                rects, full = [render.DirtyRect(0, 0, w, h)], True
                dirty_bytes, est_txns = self._plan_cost(rects, self.bpp)
                self._send_rects(rects, levels, w, h, full)

        if self.preview_path:
            self._write_preview(levels, w, h)

        self._prev_levels = levels
        stats = MirrorCycleStats(
            cycle=self._cycle, full=full, rects=len(rects), dirty_bytes=dirty_bytes,
            est_i2c_txns=est_txns, reconnected=reconnected,
            mean_luminance=luminance, invert=invert_now, threshold=threshold_value,
        )
        threshold_str = "-" if threshold_value is None else str(threshold_value)
        self.log.info(
            "frame %d: %s rects=%d dirty_bytes=%d est_i2c_txns=%d mean_lum=%.1f invert=%s threshold=%s%s",
            stats.cycle, "full" if full else "partial", stats.rects,
            stats.dirty_bytes, stats.est_i2c_txns, luminance, invert_now, threshold_str,
            " (reconnected)" if reconnected else "",
        )
        if self.dry_run and self.print_stats:
            print(
                f"frame {stats.cycle}: {'full' if full else 'partial'} "
                f"rects={stats.rects} dirty_bytes={stats.dirty_bytes} "
                f"est_i2c_txns={stats.est_i2c_txns} mean_lum={luminance:.1f} "
                f"invert={invert_now} threshold={threshold_str}"
            )
        self._cycle += 1
        return stats

    def run(self, max_cycles: Optional[int] = None) -> None:
        """Run until the helper's stream closes, Ctrl-C, or `max_cycles`
        cycles have run. A clean helper exit (code 0) or Ctrl-C returns
        normally; permission failure or any other helper exit raises."""
        try:
            while True:
                try:
                    self.run_once()
                except _HelperEOF:
                    self._handle_helper_eof()  # raises, or returns to mean "stop cleanly"
                    break
                if max_cycles is not None and self._cycle >= max_cycles:
                    break
        except KeyboardInterrupt:
            self.log.info("mirror: interrupted, stopping")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def _fps_type(s: str) -> float:
    try:
        v = float(s)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid --fps {s!r}") from exc
    if not (MIN_FPS <= v <= MAX_FPS):
        raise argparse.ArgumentTypeError(f"--fps must be between {MIN_FPS} and {MAX_FPS}, got {v}")
    return v


def _region_type(s: str) -> Tuple[int, int, int, int]:
    parts = s.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError(f"--region must be x,y,w,h, got {s!r}")
    try:
        x, y, w, h = (int(p) for p in parts)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"--region values must be integers, got {s!r}") from exc
    if w <= 0 or h <= 0 or x < 0 or y < 0:
        raise argparse.ArgumentTypeError(f"--region has invalid geometry: {s!r}")
    return (x, y, w, h)


_MAX_SCALE = 6


def _scale_type(s: str) -> int:
    try:
        v = int(s)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid --scale {s!r}") from exc
    if not (1 <= v <= _MAX_SCALE):
        raise argparse.ArgumentTypeError(f"--scale must be between 1 and {_MAX_SCALE}, got {v}")
    return v


def resolve_contextual_defaults(args: argparse.Namespace) -> None:
    """Mutate `args` in place: `--fit` and `--dither` both have a
    context-dependent default (`None` from `build_parser` means "not
    given") that can only be resolved once we know whether the source is
    `--window` or `--display`. A window capture is (per
    docs/sample-projects/mirror-and-virtual-display.md §4, and the 2026-09-03
    on-device note there) usually a
    small area of a much bigger source, so `--fit` defaults to "fill"
    (crop-to-cover, not letterbox) for `--window`, and text-heavy window
    sources default to plain `--dither none` (an ordered/error-diffusion
    dither only hurts thin glyph strokes) -- a `--display` capture keeps
    the old "contain"/"bayer" defaults, since the whole display is usually
    already close to the panel's own aspect ratio."""
    if args.fit is None:
        args.fit = "fill" if args.window is not None else "contain"
    if args.dither is None:
        args.dither = "none" if (args.fit in ("fill", "band") and args.window is not None) else "bayer"


def capture_size(target_w: int, target_h: int, fit: str, scale: int = 1) -> Tuple[int, int]:
    """The `--width`/`--height` to request from the helper for a given
    panel size, `--fit` mode and `--scale` factor.

    For `--fit fill`/`band`/`stretch`, a *square* capture (rather than the
    panel's own often-extreme aspect ratio, e.g. 3:1 for 240x80)
    minimizes the helper's own scale-to-fit letterbox for most real
    window/display aspect ratios, leaving actual content for
    `MirrorPipeline._canvas_image`'s crop-to-cover step to work with
    instead of mostly black bars -- this is what fixed the 2026-09-03
    on-device "small square in the panel centre" report
    (docs/sample-projects/mirror-and-virtual-display.md §4). `--fit contain` keeps requesting
    exactly the panel's own aspect ratio, matching its pre-existing
    behavior. `--scale` further multiplies either result for supersampled
    quality (captured at N times, downscaled with LANCZOS -- more helper
    CPU/bandwidth and a slower per-frame cycle, traded for crisper small
    text; see docs/sample-projects/mirror-and-virtual-display.md §4)."""
    if fit in ("fill", "band", "stretch"):
        side = max(target_w, target_h) * scale
        return side, side
    return target_w * scale, target_h * scale


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m byok.mirror",
        description="Mirror a Mac display or window to the BYOK panel (Mode 2).",
    )
    src = parser.add_mutually_exclusive_group()
    src.add_argument("--display", type=int, default=None, metavar="ID", help="display id from --list")
    src.add_argument("--window", type=int, default=None, metavar="ID", help="window id from --list")
    src.add_argument("--list", action="store_true", help="list available displays/windows and exit")

    parser.add_argument("--fps", type=_fps_type, default=DEFAULT_FPS, metavar="N",
                         help=f"frames/sec, {MIN_FPS}-{MAX_FPS} (default {DEFAULT_FPS})")
    parser.add_argument("--fit", choices=("contain", "fill", "stretch", "band"), default=None,
                         help="how to map the capture onto the panel (default: fill for --window, "
                              "contain for --display)")
    parser.add_argument("--band", choices=("top", "middle", "bottom"), default="middle",
                         help="which strip --fit fill/band keeps along the cropped axis (default: middle)")
    parser.add_argument("--scale", type=_scale_type, default=1, metavar="N",
                         help=f"capture at N times panel resolution and downscale with LANCZOS, 1-{_MAX_SCALE} "
                              "(default: 1; try 2-3 for small text)")
    parser.add_argument("--invert", choices=("auto", "on", "off"), default="auto",
                         help="invert dark-background frames before quantizing (default: auto, decided "
                              "per frame from mean luminance with hysteresis)")
    parser.add_argument("--threshold-method", choices=("otsu", "mean", "fixed"), default="otsu",
                         help="cut point --dither none uses when --fit is fill/band (default: otsu; "
                              "'fixed' restores the old 127.5-midpoint behavior)")
    parser.add_argument("--dither", choices=sorted(render._QUANTIZERS), default=None,
                         help="dither method (default: none for --window with --fit fill/band, "
                              "bayer otherwise)")
    parser.add_argument("--threshold", type=int, default=DEFAULT_THRESHOLD, metavar="N",
                         help="helper's changed-pixel threshold, 0-255 (default: %(default)s) -- "
                              "NOT the quantization cut point, see --threshold-method for that")
    parser.add_argument("--region", type=_region_type, default=None, metavar="x,y,w,h",
                         help="crop the captured canvas to this rect (capture pixels, which may be larger "
                              "than the panel under --fit fill/band/stretch or --scale) before re-fitting "
                              "to the panel; overrides --fit's own band/crop selection when given")
    parser.add_argument("--full-every", type=int, default=DEFAULT_FULL_EVERY, metavar="N",
                         help="force a full refresh every N frames, 0 to disable (default: %(default)s)")
    parser.add_argument("--preview", default=None, metavar="PATH",
                         help="write rendered frames as PNG to PATH (a single overwritten file normally; "
                              "a directory of numbered frames under --dry-run)")
    parser.add_argument("--dry-run", action="store_true",
                         help="run the pipeline without a device: print per-frame stats, send nothing")
    parser.add_argument("--bpp", type=int, choices=(1, 2), default=None,
                         help="default: the connected device's native bpp (dry-run default: 1)")
    parser.add_argument("--port", default=None, help="serial device (default: auto-discover)")
    parser.add_argument("--max-cycles", type=int, default=None, help=argparse.SUPPRESS)
    parser.add_argument("--verbose", "-v", action="count", default=0, help="-v for INFO, -vv for DEBUG")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)

    level = logging.WARNING
    if args.verbose == 1:
        level = logging.INFO
    elif args.verbose >= 2:
        level = logging.DEBUG
    logging.basicConfig(level=level, format="%(levelname)s %(name)s: %(message)s")

    try:
        helper_path = ensure_helper_built()
    except HelperError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.list:
        try:
            print(run_list([helper_path]), end="")
        except HelperPermissionError as exc:
            print(str(exc), file=sys.stderr)
            return 3
        except HelperError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        return 0

    if args.display is None and args.window is None:
        print("error: --display ID or --window ID is required (see --list)", file=sys.stderr)
        return 2

    resolve_contextual_defaults(args)

    device: Optional[Device] = None
    if not args.dry_run:
        try:
            device = Device.open(port=args.port)
        except (WrongDevice, NoDeviceFound, HandshakeFailed, TransportError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        target_w, target_h = device.display_size
        bpp = args.bpp if args.bpp is not None else device.native_bpp
    else:
        target_w, target_h = DEFAULT_WIDTH, DEFAULT_HEIGHT
        bpp = args.bpp if args.bpp is not None else 1

    capture_w, capture_h = capture_size(target_w, target_h, args.fit, args.scale)

    try:
        proc = spawn_capture(
            [helper_path], display=args.display, window=args.window,
            width=capture_w, height=capture_h, fps=args.fps, threshold=args.threshold,
        )
    except HelperError as exc:
        print(f"error: {exc}", file=sys.stderr)
        if device is not None:
            device.close()
        return 1

    pipeline = MirrorPipeline(
        device=device, stream=proc.stdout, width=target_w, height=target_h, bpp=bpp,
        dither=args.dither, region=args.region, fit=args.fit, band=args.band,
        invert=args.invert, threshold_method=args.threshold_method,
        fps=args.fps, full_every=args.full_every,
        preview_path=args.preview, dry_run=args.dry_run, process=proc,
    )
    rc = 0
    try:
        pipeline.run(max_cycles=args.max_cycles)
    except HelperPermissionError as exc:
        print(str(exc), file=sys.stderr)
        rc = 3
    except HelperError as exc:
        print(f"error: {exc}", file=sys.stderr)
        rc = 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        rc = 130
    finally:
        terminate_process(proc)
        if device is not None:
            device.close()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
