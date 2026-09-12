"""`byok notify` -- shared state file + drawing helpers.

**The design problem this solves.** `byok notify TEXT` needs to put a
short-lived inverted banner across the top of the panel and then restore
whatever was there, but the BYOK Link is a single-consumer serial port
(`transport.py`'s module docstring) -- only one process can hold it open
at a time. If a `byok dashboard` loop is already running in another
process (the common case: a long-lived dashboard, with `notify` fired
from some other script or automation), a second process cannot also open
the port to draw its own banner. So `notify` doesn't try to own the display
in that case: it drops a small JSON *request* file at `NOTIFY_PATH`
(under `~/.cache/byok/` -- our own cache directory) and the
*already-running* loop
(`dashboard/loop.py`'s `DashboardLoop`, which polls this file once per
render cycle -- see its own docstring) is the one that actually draws
and restores it.

`cli.py cmd_notify` tries a direct device connection first (cheap to
attempt, and correct when nothing else is running); if that fails
because the port is busy, it falls back to writing the request file
instead and tells the operator it queued for the running loop.

**Two different drawing paths, deliberately.** The standalone (no loop)
path in `cli.py` draws the banner as literal
`DRAW_RECT`(filled, dark)/`DRAW_TEXT`(inverted style)/`PARTIAL_REFRESH`
device commands -- the device's own built-in font, no host-side
rendering pipeline involved, and restore is a `DRAW_RECT`(clear region)
+ `PARTIAL_REFRESH` back to background. The loop-mediated path instead
uses `overlay_banner()` below to draw directly onto the *composite PIL
image* `DashboardLoop.run_once()` already builds each cycle, **before**
that cycle's normal (already dirty-rect-diffed, already
`PARTIAL_REFRESH`-driving) send. This is a deliberate choice, not an
inconsistency: the loop tracks `_prev_levels` (its own belief of what's
currently on the device) so it can diff each new frame against it and
send only what changed. A raw `DRAW_TEXT` overlay drawn independently of
that composite would make the device's *actual* pixels diverge from
`_prev_levels` without the loop knowing -- the very next cycle's diff
could then decide "this region didn't change" (because the *composite*
didn't change there) and skip resending it, leaving stale banner pixels
on the panel forever instead of restoring them. Baking the banner into
the composite itself keeps `_prev_levels` truthful, so a later cycle
naturally computes "banner region changed back to real content" as a
dirty rect and sends it via the loop's existing `PARTIAL_REFRESH` path --
still `PARTIAL_REFRESH`-driven, just composited rather than layered.

**Restore timing, loop-mediated path.** The loop only checks this file
once per render cycle (`refresh_seconds`), not on a separate timer -- see
`DashboardLoop`'s own docstring for why its cadence logic isn't
restructured for this. A banner requested with `--seconds` shorter than
the running dashboard's `refresh_seconds` restores late, on the next
cycle after expiry, not exactly on time. Documented in
`docs/host-tools.md`'s "Notify" section, not silently accepted as
"good enough" without saying so.
"""

from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from typing import Optional

from PIL import Image, ImageDraw

DEFAULT_NOTIFY_PATH = os.path.expanduser("~/.cache/byok/notify.json")
DEFAULT_SECONDS = 5.0
DEFAULT_BANNER_HEIGHT = 16  # px; clamped to at most height // 3 by overlay_banner()

# Same directory as DEFAULT_NOTIFY_PATH, same atomic-write convention --
# see write_lock()'s docstring for what this file is and who reads it.
DEFAULT_LOCK_PATH = os.path.expanduser("~/.cache/byok/loop.lock")


@dataclass(frozen=True)
class LoopLock:
    pid: int
    started_at: float  # unix seconds


def write_lock(path: str = DEFAULT_LOCK_PATH, pid: Optional[int] = None) -> LoopLock:
    """Records that a long-lived port-owning loop (`byok dashboard`,
    `byok mirror`) is running, so `cli.py`'s `cmd_notify` can go straight
    to the IPC request file (`write_request` below) instead of attempting
    -- and, since `transport.py`'s serial factory now opens with
    `exclusive=True`, failing -- to open the device itself. Written once
    at loop startup (`dashboard.loop.DashboardLoop.run`) and removed on
    clean shutdown (`clear_lock`, same `finally` as the device close).

    `pid` defaults to this process's own pid. Atomic write (temp file +
    `os.replace`), same convention as `write_request` -- a reader never
    sees a partial file."""
    pid = os.getpid() if pid is None else pid
    lock = LoopLock(pid=pid, started_at=time.time())
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump({"pid": lock.pid, "started_at": lock.started_at}, fh)
    os.replace(tmp, path)
    return lock


def read_lock(path: str = DEFAULT_LOCK_PATH) -> Optional[LoopLock]:
    """Never raises -- a missing/corrupt/racy-partial-write file just
    means "no loop lock on record", same degrade-gracefully convention
    as `read_request`."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        return LoopLock(pid=int(data["pid"]), started_at=float(data["started_at"]))
    except (OSError, ValueError, KeyError, TypeError):
        return None


def clear_lock(path: str = DEFAULT_LOCK_PATH) -> None:
    """Best-effort delete -- called once on a loop's clean shutdown. A
    failed delete here is not itself a bug (mirrors `clear_request`):
    the pid check in `loop_is_running` still catches a stale lock left
    behind by an unclean exit (SIGKILL, power loss)."""
    try:
        os.remove(path)
    except OSError:
        pass


def _pid_alive(pid: int) -> bool:
    """True if `pid` names a process this machine can currently see.
    `os.kill(pid, 0)` sends no signal -- it only asks the kernel whether
    the pid exists and is signalable -- so this never affects whatever
    process it finds."""
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # Exists (owned by someone else) -- not a case this lock file
        # should ever actually hit, since only our own loop writes it,
        # but "exists" is the correct answer regardless.
        return True
    return True


def loop_is_running(path: str = DEFAULT_LOCK_PATH) -> bool:
    """True if `path` names a live dashboard/mirror loop -- see
    `write_lock`'s docstring for who writes this and when. A stale lock
    (the process that wrote it died without reaching its `clear_lock`,
    e.g. SIGKILL) reads as not-running once its pid is gone, so a
    crashed loop never permanently strands `cmd_notify` in IPC-only
    mode."""
    lock = read_lock(path)
    if lock is None:
        return False
    return _pid_alive(lock.pid)


@dataclass(frozen=True)
class NotifyRequest:
    text: str
    requested_at: float  # unix seconds -- also this request's identity (id)
    expires_at: float    # unix seconds


def write_request(text: str, seconds: float = DEFAULT_SECONDS, path: str = DEFAULT_NOTIFY_PATH,
                   now: Optional[float] = None) -> NotifyRequest:
    """Atomically writes a new banner request, replacing any prior one
    (the loop only ever needs to know about the latest request -- a
    banner mid-display when a second `notify` fires is simply replaced,
    not queued)."""
    now = time.time() if now is None else now
    req = NotifyRequest(text=text, requested_at=now, expires_at=now + max(0.0, seconds))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump({"text": req.text, "requested_at": req.requested_at, "expires_at": req.expires_at}, fh)
    os.replace(tmp, path)
    return req


def read_request(path: str = DEFAULT_NOTIFY_PATH) -> Optional[NotifyRequest]:
    """Never raises -- a missing/corrupt/racy-partial-write file just
    means "no pending banner", same degrade-gracefully convention as
    every widget Provider in this package."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        return NotifyRequest(
            text=str(data["text"]),
            requested_at=float(data["requested_at"]),
            expires_at=float(data["expires_at"]),
        )
    except (OSError, ValueError, KeyError, TypeError):
        return None


def clear_request(path: str = DEFAULT_NOTIFY_PATH) -> None:
    """Best-effort delete -- called once a request has been fully shown
    and restored, so a later poll doesn't keep re-noticing an already-
    handled (expired) request. Not calling this promptly is harmless
    (an expired request is simply never "active" again -- see
    `is_active()`), so a failed delete here is not itself a bug."""
    try:
        os.remove(path)
    except OSError:
        pass


def is_active(req: Optional[NotifyRequest], now: Optional[float] = None) -> bool:
    if req is None:
        return False
    now = time.time() if now is None else now
    return req.requested_at <= now < req.expires_at


def overlay_banner(image: Image.Image, text: str, fonts=None, height: Optional[int] = None) -> None:
    """Draws an inverted banner (filled dark background, light text)
    across the top rows of `image`, in place. `image` is expected to be
    the dashboard loop's own rendered composite ('L' mode) -- see module
    docstring for why this is composited rather than layered as separate
    device commands for the loop-mediated path.

    `height` defaults to `DEFAULT_BANNER_HEIGHT`, clamped to at most
    `image.height // 3` so a banner can never dominate (or exceed) a
    very short panel.
    """
    w, h = image.size
    bh = min(height or DEFAULT_BANNER_HEIGHT, max(1, h // 3))
    draw = ImageDraw.Draw(image)
    draw.rectangle([0, 0, w - 1, bh - 1], fill=0)

    from .dashboard.fonts import FontSet, apply_text_case, ellipsize, fit_font_size

    fonts = fonts or FontSet()

    class _Ctx:
        # A minimal stand-in for RenderContext -- apply_text_case() only
        # reads .height and .options off it (fonts.py's resolve_text_case),
        # and this banner always wants the same "small row -> upper"
        # legibility default every other 16px row in this codebase gets.
        height = bh
        options: dict = {}

    upper_text = apply_text_case(text, _Ctx())
    font = fit_font_size(draw, upper_text, fonts, max(1, w - 4), max(1, bh - 4), min_size=6, max_size=max(7, bh))
    upper_text = ellipsize(draw, upper_text, font, max(1, w - 4))
    draw.text((2, max(0, (bh - _text_h(draw, upper_text, font)) // 2)), upper_text, font=font, fill=255)


def _text_h(draw, text, font) -> int:
    from .dashboard.fonts import text_size

    return text_size(draw, text, font)[1]
