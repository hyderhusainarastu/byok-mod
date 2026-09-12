"""The `byok dashboard` render/send loop.

`DashboardLoop` owns the steady-state cycle: render the config to an image,
quantize it, and get it onto the panel -- full-frame the first time (and
every `full_every`th time after), a dirty-rect diff otherwise. It knows
nothing about argparse or `_connect()`; `cli.py`'s `cmd_dashboard` wires a
real `Device` and a fixed argv into it. Every I/O dependency (`Device`,
`now_fn`, `clock`, `sleep`) is a constructor parameter so
`tests/host/test_dashboard_loop.py` can drive it with a fake transport and
a manual clock -- no real serial port, no real wall-clock sleep.

Per-cycle wire strategy (docs/protocol.md §6.3, docs/display.md):

  * First frame, and every `full_every`th frame after (default 60): whole
    panel via `FRAME_BEGIN`/`FRAME_DATA`/`FRAME_END` + `FULL_REFRESH`
    (`Device.image()`'s own path, ~220 ms on real hardware per this
    project's measurements).
  * Every other frame: diff against the previous packed frame
    (`byok.render.choose_dirty_plan`, page-snapped per docs/display.md's
    8-row partial-refresh granularity) and send only what changed --
    `DRAW_BITMAP` (a rect small enough for one frame, the common case at
    240x80x1bpp) or the `FRAME_BEGIN(partial)/FRAME_DATA/FRAME_END`
    sequence (larger rects only) -- followed by an explicit
    `PARTIAL_REFRESH` for that rect.
  * A `TransportError` (link dropped) during a cycle triggers
    `transport.connect_with_backoff()` and forces the *next* successful
    send to be a full frame (the panel's actual on-screen state across a
    reconnect is unknown).

Two more things happen once per cycle, both added in v1.2, both before
the render/send above:

  * **Device events** (`SerialTransport.poll_events()`, non-blocking): an
    `EVT_PRESET_CHANGED` event, or the preset-index bits of an
    `EVT_STATUS`'s `flags` byte (docs/protocol.md §6.1/§6.4b/§6.5, v1.2 --
    reconciled from an earlier host-side PROPOSED design, see
    `proto.py`'s `Type` enum comment), switches `self.config` to the
    named preset (`presets=` constructor arg, `byok.dashboard.presets`)
    and forces the next frame full, since the device's own on-screen
    state for a whole different config is unknown; an `EVT_BUTTON` event
    (docs/protocol.md §6.5, unchanged wire format across this
    reconciliation) is handed to `on_button` if one was given -- `cli.py`
    wires `dashboard/media_transport.py`'s handler there for
    UP/DOWN/BRIGHTNESS transport control, gated to only act while the
    "media" preset (`active_preset_name`) is current.
  * **The notify banner** (`byok.notify_ipc`, `byok notify` on the CLI):
    if a request in `~/.cache/byok/notify.json` is currently active, it's
    composited onto this cycle's rendered frame before quantization
    (`notify_ipc.overlay_banner`) -- see that module's own docstring for
    why baking it into the composite, not issuing a separate `DRAW_TEXT`
    overlay, is what keeps `_prev_levels` (and thus every later cycle's
    dirty-rect diff) truthful about what's actually on the device.
"""

from __future__ import annotations

import datetime as _dt
import logging
import time
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional

from .. import notify_ipc
from .. import proto
from .. import render as render_mod
from .. import device as device_mod
from ..device import Device
from ..transport import TransportError
from . import presets as presets_mod
from . import render as dash_render
from .config import DashboardConfig
from .fonts import FontSet

logger = logging.getLogger("byok.dashboard.loop")

# DRAW_BITMAP's header is 10 B (§6.2); anything at or under this many packed
# pixel bytes fits one DRAW_BITMAP frame. Above it, a dirty rect goes out via
# the FRAME_BEGIN(partial)/FRAME_DATA/FRAME_END sequence instead.
_MAX_DRAW_BITMAP_PIXELS = proto.MAX_PAYLOAD - 10

# Vertical partial-refresh granularity (docs/display.md): the controller
# addresses rows in 8-row pages.
DEFAULT_PAGE_ROWS = 8

DEFAULT_FULL_EVERY = 60


@dataclass
class CycleStats:
    """What one `DashboardLoop.run_once()` call did, for logging/tests."""

    cycle: int
    render_ms: float
    bytes_sent: int
    rects: int
    full: bool
    reconnected: bool = False


class DashboardLoop:
    def __init__(
        self,
        device: Device,
        config: DashboardConfig,
        *,
        interval: Optional[float] = None,
        full_every: int = DEFAULT_FULL_EVERY,
        preview_path: Optional[str] = None,
        providers: Optional[Dict[str, Any]] = None,
        fonts: Optional[FontSet] = None,
        now_fn: Callable[[], _dt.datetime] = _dt.datetime.now,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
        page_rows: int = DEFAULT_PAGE_ROWS,
        reconnect_max_attempts: Optional[int] = None,
        log: Optional[logging.Logger] = None,
        presets: Optional[List["presets_mod.PresetEntry"]] = None,
        on_button: Optional[Callable[["device_mod.ButtonEvent"], None]] = None,
        notify_path: Optional[str] = notify_ipc.DEFAULT_NOTIFY_PATH,
        lock_path: Optional[str] = notify_ipc.DEFAULT_LOCK_PATH,
    ):
        self.device = device
        self.config = config
        self.interval = interval if interval is not None else float(config.refresh_seconds)
        self.full_every = full_every
        self.preview_path = preview_path
        self.providers = providers or {}
        self.fonts = fonts or FontSet(ttf_path=config.fonts.get("path"))
        self.now_fn = now_fn
        self.clock = clock
        self.sleep = sleep
        self.page_rows = page_rows
        self.reconnect_max_attempts = reconnect_max_attempts
        self.log = log or logger

        # Presets (byok.dashboard.presets), live-switched via PRESET_CHANGED
        # -- see class/module docstring. `None` (the default) means this
        # loop just runs the one `config` it was given, exactly as it did
        # before presets existed; a non-empty list opts into event-driven
        # switching.
        self.presets = list(presets) if presets else None
        # cli.py's `--preset NAME` sets this True after force_preset()'ing
        # the requested preset: the operator asked for exactly one
        # preset, so a later PRESET_CHANGED (e.g. from the device's own
        # physical menu) is ignored rather than silently overriding an
        # explicit choice. `self.presets` itself stays populated in that
        # mode (not cleared) so `active_preset_name` -- and therefore
        # `media_transport`'s "is the media preset active" check -- keeps
        # working exactly the same whether pinned or freely switching.
        self.preset_switching_locked = False
        self.on_button = on_button
        self.notify_path = notify_path
        # `byok.notify_ipc` loop lock (write_lock/clear_lock): held for the
        # duration of `run()` so `cli.py`'s `cmd_notify` can tell a loop
        # already owns the port and go straight to the IPC request file --
        # see `run()`'s docstring and `notify_ipc.write_lock`'s own.
        # `None` opts out entirely (tests that call `run_once()` directly
        # never touch this; tests of `run()` itself get the real default
        # path, same convention `notify_path` above already uses).
        self.lock_path = lock_path
        self._preset_configs: Dict[int, DashboardConfig] = {}
        self._active_preset_index: Optional[int] = None
        self._notify_active_req: Optional["notify_ipc.NotifyRequest"] = None

        # Panel geometry always comes from the live HELLO_ACK, never the
        # config (which is geometry-agnostic by design -- see layout.py) --
        # this is what actually renders and what byok dashboard is a
        # command *for*, so it must match the connected device exactly.
        self.width, self.height = device.display_size
        self.bpp = config.display.bpp
        if self.bpp == 2 and not (device.hello_ack.caps & 0x02):
            self.log.warning("device does not advertise 2bpp support (caps 0x%02X); using %dbpp",
                              device.hello_ack.caps, device.native_bpp)
            self.bpp = device.native_bpp

        self._prev_levels: Optional[render_mod.Levels] = None
        self._cycle = 0

    @property
    def active_preset_name(self) -> Optional[str]:
        """The short `name` (not `display`) of the currently-active preset,
        or `None` if this loop isn't preset-driven (`presets=` wasn't
        given) or no `PRESET_CHANGED`/forced selection has set one yet.
        `dashboard/media_transport.py`'s button handler reads this to
        gate UP/DOWN/BRIGHTNESS transport control to only the "media"
        preset."""
        if self.presets is None or self._active_preset_index is None:
            return None
        return self.presets[self._active_preset_index].name

    def force_preset(self, index: int) -> None:
        """Switch to `presets[index]` right now (used by `cli.py` for
        `--preset NAME`'s initial selection, and by tests) -- same effect
        as an incoming `PRESET_CHANGED` event naming that index. Also
        respects `preset_switching_locked` (so calling this after locking
        is a no-op, same as an ignored `PRESET_CHANGED`) -- `cli.py` sets
        the lock *after* this call for `--preset`'s own initial
        selection, never before."""
        self._switch_preset(index)

    # -- one cycle ----------------------------------------------------------

    def _switch_preset(self, index: int) -> None:
        if self.preset_switching_locked:
            self.log.debug("preset switching locked (--preset was forced); ignoring index %d", index)
            return
        if not self.presets:
            # Not preset-driven at all (--config mode) -- debug, not
            # warning: EVT_STATUS's preset-index bits (§6.1/§6.4b) are
            # sent by any v1.2 device regardless of whether this host
            # asked for preset switching, at up to ~1 Hz (§6.5), so this
            # is routine background traffic here, not something worth a
            # log line by default every cycle.
            self.log.debug("preset index %d reported but no presets configured; ignoring", index)
            return
        if not (0 <= index < len(self.presets)):
            self.log.warning(
                "preset index %d out of range (have %d presets); ignoring",
                index, len(self.presets),
            )
            return
        if index == self._active_preset_index:
            return  # already showing it -- not a real change
        entry = self.presets[index]
        cfg = self._preset_configs.get(index)
        if cfg is None:
            cfg = presets_mod.load_preset_config(entry)
            self._preset_configs[index] = cfg
        self.config = cfg
        self._active_preset_index = index
        # The panel's actual current pixels belong to whatever config was
        # showing before -- diffing the new config's first frame against
        # that stale `_prev_levels` would send nothing for any region that
        # happens to render the same bytes by coincidence. Force a full
        # frame instead, exactly like a reconnect does (_reconnect()
        # below has the same `_prev_levels = None` line for the same
        # reason).
        self._prev_levels = None
        self.log.info("preset changed -> %s (index %d)", entry.name, index)

    def _handle_device_events(self) -> None:
        """Polls `SerialTransport.poll_events()` (non-blocking) once, and
        dispatches whatever `EVENT` frames came back -- see class
        docstring's "Device events" paragraph. Never raises: an
        unreadable/malformed event payload is logged and skipped, not
        fatal to the cycle (matches this whole module's "one bad thing
        never blanks the dashboard" posture)."""
        try:
            events = self.device.transport.poll_events()
        except Exception:  # noqa: BLE001 - a dead link here is handled by
            # the cycle's own normal send-path TransportError handling;
            # poll_events() failing silently just means "no events seen
            # this cycle", not a reason to abort early.
            self.log.debug("poll_events() failed", exc_info=True)
            return

        for frame in events:
            if frame.type == proto.Type.EVT_PRESET_CHANGED:
                try:
                    index = device_mod._decode_evt_preset_changed(frame.payload)
                except ValueError:
                    self.log.warning("malformed EVT_PRESET_CHANGED payload, ignoring")
                    continue
                self._switch_preset(index)
            elif frame.type == proto.Type.EVT_STATUS:
                # docs/protocol.md §6.1/§6.4b: EVT_STATUS's `flags` byte
                # carries the currently-selected preset index in bits
                # 4-6 as of v1.2 -- a second, redundant path to the same
                # information EVT_PRESET_CHANGED already reports
                # (explicitly asked for: "reads the selected index from
                # STATUS / PRESET_CHANGED events"). `_switch_preset` is a
                # no-op if this names the already-active preset, so
                # EVT_STATUS's own ~1 Hz rate limit (§6.5) never causes
                # redundant full-frame forces.
                try:
                    status = device_mod._decode_status(frame.payload)
                except ValueError:
                    self.log.warning("malformed EVT_STATUS payload, ignoring")
                    continue
                self._switch_preset(device_mod.preset_index_from_status_flags(status.flags))
            elif frame.type == proto.Type.EVT_BUTTON:
                if self.on_button is None:
                    continue
                try:
                    button_event = device_mod._decode_evt_button(frame.payload)
                except ValueError:
                    self.log.warning("malformed EVT_BUTTON payload, ignoring")
                    continue
                try:
                    self.on_button(button_event)
                except Exception:  # noqa: BLE001 - a broken button handler
                    # must not take the render loop down with it.
                    self.log.warning("on_button handler raised", exc_info=True)

    def _apply_notify_overlay(self, composite) -> None:
        """Polls `~/.cache/byok/notify.json` (`byok.notify_ipc`) once, and
        composites the banner onto `composite` in place if a request is
        currently active -- see class/module docstring for why this cycle
        composites rather than issuing a separate device command. A
        request that just expired (was active last cycle, isn't now) is
        cleared from disk here so it isn't re-noticed forever; simply not
        overlaying *is* the restore -- `composite` already reflects this
        cycle's real content."""
        if not self.notify_path:
            return
        req = notify_ipc.read_request(self.notify_path)
        now = time.time()
        if notify_ipc.is_active(req, now=now):
            try:
                notify_ipc.overlay_banner(composite, req.text, fonts=self.fonts)
            except Exception:  # noqa: BLE001 - a banner draw failure must
                # not cost the cycle its normal frame.
                self.log.warning("notify overlay failed", exc_info=True)
            self._notify_active_req = req
        else:
            self._notify_active_req = None
            if req is not None:
                # Not active -- either just expired (was active last
                # cycle) or was already expired the very first time this
                # loop ever polled it (e.g. a very short --seconds that
                # elapsed between `byok notify` writing it and this
                # cycle's poll). Either way it will never become active
                # again (is_active() is monotonic in time), so clear it
                # now rather than only on a true active->inactive
                # transition -- an already-dead request left on disk
                # would otherwise linger there forever, harmlessly but
                # untidily.
                notify_ipc.clear_request(self.notify_path)

    def run_once(self) -> CycleStats:
        t0 = self.clock()
        self._handle_device_events()
        now = self.now_fn()
        composite = dash_render.render_and_quantize(
            self.config, self.width, self.height, self.bpp, now,
            providers=self.providers, fonts=self.fonts,
        )
        self._apply_notify_overlay(composite)
        # The composite's pixel values are already exactly the levels the
        # target bit depth can represent (see dashboard/render.py's
        # docstring); "none" here just reads those values back out as level
        # indices -- it is not a second, lossy quantization pass.
        levels, w, h = render_mod.quantize_levels(composite, levels=2 ** self.bpp, method="none")
        render_ms = (self.clock() - t0) * 1000.0

        force_full = self._prev_levels is None or (
            self.full_every > 0 and self._cycle % self.full_every == 0
        )

        reconnected = False
        try:
            if force_full:
                rects, bytes_sent = self._send_full(levels, w, h)
                full = True
            else:
                rects, bytes_sent = self._send_dirty(self._prev_levels, levels, w, h)
                full = False
        except (TransportError, device_mod.NackReceived) as exc:
            # NackReceived here means a *non*-E_SEQ_GAP NACK reached this
            # far (transport.request() already absorbs E_SEQ_GAP itself --
            # see its own docstring -- so this is a genuine protocol-level
            # refusal: E_STATE from a still-open frame transaction after a
            # previous cycle was cut off mid-stream, E_BUSY, etc). Treated
            # the same as a dropped link rather than left to kill the loop
            # (device.py:36's NackReceived is a bare Exception, so before
            # this it propagated straight out of run_once() and, via
            # run(), out of the whole process -- exactly the failure mode
            # incident 2026-09-04 hit): reconnect (fresh HELLO clears
            # whatever state the device was confused about) and retry this
            # cycle as a full frame, same recovery as a TransportError.
            self.log.warning("cycle %d: link error (%s); reconnecting", self._cycle, exc)
            self._reconnect()
            reconnected = True
            # self._reconnect() may have re-read self.width/self.height from
            # a new HELLO_ACK -- re-render against whatever geometry is now
            # current before retrying, rather than resending the frame that
            # was rendered (and quantized) against the pre-reconnect size.
            if (w, h) != (self.width, self.height):
                composite = dash_render.render_and_quantize(
                    self.config, self.width, self.height, self.bpp, now,
                    providers=self.providers, fonts=self.fonts,
                )
                self._apply_notify_overlay(composite)
                levels, w, h = render_mod.quantize_levels(
                    composite, levels=2 ** self.bpp, method="none"
                )
            # Device state across the reconnect is unknown -- the retry
            # (and every cycle after it, until forced again) must be full.
            rects, bytes_sent = self._send_full(levels, w, h)
            full = True

        if self.preview_path:
            composite.save(self.preview_path)

        self._prev_levels = levels
        stats = CycleStats(
            cycle=self._cycle, render_ms=render_ms, bytes_sent=bytes_sent,
            rects=rects, full=full, reconnected=reconnected,
        )
        self.log.info(
            "cycle %d: render=%.1fms bytes=%d rects=%d %s%s",
            stats.cycle, stats.render_ms, stats.bytes_sent, stats.rects,
            "full" if stats.full else "partial",
            " (reconnected)" if stats.reconnected else "",
        )
        self._cycle += 1
        return stats

    def run(self, once: bool = False, max_cycles: Optional[int] = None) -> None:
        """Run the cycle loop. Returns normally on Ctrl-C (KeyboardInterrupt)
        or when `once`/`max_cycles` ends it -- always closes the device's
        transport on the way out.

        Cadence is wall-clock aligned (docs/troubleshooting.md
        §4/§5.2): each cycle targets `next_tick = start + n * interval`, not
        `interval` tacked on *after* however long `run_once()` took. A flat
        post-work sleep makes the real frame period `interval +
        run_once()'s cost`; against this project's ~390-430ms `mac_stats`
        cost that aliased a requested 1s cadence into a displayed 2s/1s/2s/1s
        tick pattern. If a cycle runs so long it blows past one or more
        upcoming ticks, we log it and resync to the next tick that is still
        in the future rather than firing a burst of back-to-back catch-up
        cycles.

        Also holds `self.lock_path` (`byok.notify_ipc.write_lock`, default
        `~/.cache/byok/loop.lock`) for the run's duration -- see that
        function's own docstring for who reads it and why. Best-effort:
        a failure to write or clear it is logged, not fatal -- a missing
        lock just means `cmd_notify` falls back to its own `PortBusy`/
        direct-open handling instead of the fast IPC-first path.
        """
        if self.lock_path:
            try:
                notify_ipc.write_lock(self.lock_path)
            except OSError:
                self.log.warning("could not write loop lock %s", self.lock_path, exc_info=True)
        try:
            next_tick = self.clock()
            while True:
                self.run_once()
                if once:
                    break
                if max_cycles is not None and self._cycle >= max_cycles:
                    break
                next_tick += self.interval
                now = self.clock()
                if now > next_tick:
                    behind = now - next_tick
                    missed = int(behind // self.interval) + 1 if self.interval > 0 else 0
                    self.log.warning(
                        "cycle %d: run_once() overran interval by %.3fs; skipping "
                        "%d missed tick(s) to resync to wall clock",
                        self._cycle - 1, behind, missed,
                    )
                    next_tick += missed * self.interval
                self.sleep(max(0.0, next_tick - now))
        except KeyboardInterrupt:
            self.log.info("dashboard: interrupted, stopping")
        finally:
            if self.lock_path:
                notify_ipc.clear_lock(self.lock_path)
            try:
                self.device.close()
            except Exception:  # noqa: BLE001 - best-effort on teardown
                self.log.debug("error closing device", exc_info=True)

    # -- sending --------------------------------------------------------

    def _send_full(self, levels: render_mod.Levels, w: int, h: int) -> "tuple[int, int]":
        packed = render_mod.pack_framebuffer(levels, w, h, self.bpp)
        self.device._send_frame(w, h, self.bpp, packed, refresh=device_mod.FULL_REFRESH_MODE)
        return 1, len(packed)

    def _send_dirty(
        self, prev: render_mod.Levels, curr: render_mod.Levels, w: int, h: int
    ) -> "tuple[int, int]":
        rects, _is_bounding = render_mod.choose_dirty_plan(
            prev, curr, w, h, self.bpp, page=self.page_rows
        )
        total_bytes = 0
        for rect in rects:
            cropped = render_mod.crop_levels(curr, w, rect)
            packed = render_mod.pack_framebuffer(cropped, rect.w, rect.h, self.bpp)
            total_bytes += len(packed)
            if len(packed) <= _MAX_DRAW_BITMAP_PIXELS:
                # DRAW_BITMAP writes the back buffer only (§6.2) -- the
                # explicit refresh() call after it is what actually pushes
                # these pixels to the panel, per docs/protocol.md's refresh
                # trigger list. This is the common case at 240x80x1bpp: even
                # a *full*-panel frame (2400 B) fits one DRAW_BITMAP.
                self.device.draw_bitmap(rect.x, rect.y, rect.w, rect.h, self.bpp, packed, op=0)
                self.device.refresh(rect=(rect.x, rect.y, rect.w, rect.h))
            else:
                # Oversized rect (only possible at 2bpp, or a very large
                # bounding box): stream it via the v1 partial-frame quirk
                # (docs/protocol.md §6.3's "v1 note") instead.
                self.device._send_frame(
                    rect.w, rect.h, self.bpp, packed,
                    refresh=device_mod.PARTIAL_REFRESH_MODE, origin=(rect.x, rect.y),
                )
        return len(rects), total_bytes

    def _reconnect(self) -> None:
        transport = self.device.transport
        try:
            transport.close()
        except Exception:  # noqa: BLE001 - best-effort before reconnecting
            self.log.debug("error closing transport before reconnect", exc_info=True)
        transport.connect_with_backoff(do_hello=True, max_attempts=self.reconnect_max_attempts)
        # A HELLO_ACK could in principle report different geometry after a
        # reconnect (different boot slot, different firmware); re-read it
        # rather than assume the old values still hold.
        self.width, self.height = self.device.display_size
        self._prev_levels = None
