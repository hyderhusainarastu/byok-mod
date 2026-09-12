"""tests/host/test_dashboard_loop.py -- unittest suite for
byok.dashboard.loop.DashboardLoop.

Runnable (needs Pillow + PyYAML in the same interpreter -- see
byok/dashboard/REQUIREMENTS.md):

    python3 -m unittest tests.host.test_dashboard_loop -v

Every test drives a real `byok.device.Device` wrapping a real
`byok.transport.SerialTransport`, against a fake serial port
(`_fake_transport.RecordingSerial`) that decodes outbound frames with the
real `byok.proto.Parser` and answers them generically -- never a real
serial port, never real wall-clock sleep (a `ManualClock` drives every
timeout/backoff). This is deliberately an integration-level test: it
exercises the *actual* bundled `default_dashboard_240x80.yaml` config and
*actual* Pillow font rendering (real system fonts, present on any macOS
this project runs on), not a synthetic pixel array, so what it proves
about dirty-rect sizing and page-snapping is what the shipped config will
really do.
"""

import datetime
import os
import struct
import sys
import tempfile
import unittest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from byok import proto  # noqa: E402
from byok import render as render_mod  # noqa: E402
from byok.dashboard import config as config_mod  # noqa: E402
from byok.dashboard.loop import DashboardLoop  # noqa: E402
from byok.transport import LinkDead, TransportError  # noqa: E402

from _fake_transport import RecordingSerial, connected_device  # noqa: E402

_CONFIG_PATH = config_mod.default_config_path()


def _seconds_ticker(start=datetime.datetime(2026, 9, 3, 14, 32, 7)):
    """Returns a zero-arg callable that returns `start`, then `start` + 1s,
    + 2s, ... on each successive call -- a deterministic stand-in for
    `datetime.datetime.now` that guarantees the clock widget's `%S` field
    (and therefore its rendered text) changes every single call."""
    state = {"n": -1}

    def _now():
        state["n"] += 1
        return start + datetime.timedelta(seconds=state["n"])

    return _now


def _frames_of_type(serial: RecordingSerial, msg_type: int):
    return [f for f in serial.written_frames if f.type == msg_type]


def _draw_bitmap_rects(serial: RecordingSerial):
    """[(x, y, w, h, bpp, op, pixel_len), ...] for every DRAW_BITMAP frame
    written so far, decoded per docs/protocol.md §6.2."""
    out = []
    for f in _frames_of_type(serial, proto.Type.DRAW_BITMAP):
        x, y, w, h, bpp, op = struct.unpack_from("<HHHHBB", f.payload, 0)
        out.append((x, y, w, h, bpp, op, len(f.payload) - 10))
    return out


def _partial_refresh_rects(serial: RecordingSerial):
    out = []
    for f in _frames_of_type(serial, proto.Type.PARTIAL_REFRESH):
        out.append(struct.unpack_from("<HHHH", f.payload, 0))
    return out


def _make_loop(device, providers=None, **kwargs):
    cfg = config_mod.load(_CONFIG_PATH)
    kwargs.setdefault("interval", 1.0)
    kwargs.setdefault("now_fn", _seconds_ticker())
    # Never let the suite touch the owner's live ~/.cache/byok/ files --
    # both default to a real path unless a test explicitly overrides them
    # (see LoopLockTests, which passes an explicit tempdir).
    kwargs.setdefault("lock_path", None)
    kwargs.setdefault("notify_path", None)
    return DashboardLoop(device, cfg, providers=providers or {}, **kwargs)


class FirstFrameTests(unittest.TestCase):
    def test_first_frame_is_a_full_frame_transaction(self):
        device, serials, _clock = connected_device()
        loop = _make_loop(device)

        stats = loop.run_once()

        self.assertTrue(stats.full)
        self.assertEqual(stats.rects, 1)
        self.assertEqual(stats.cycle, 0)
        serial = serials["serial"]
        # Whole-panel FRAME_BEGIN/FRAME_DATA/FRAME_END, no DRAW_BITMAP at all.
        self.assertEqual(len(_frames_of_type(serial, proto.Type.FRAME_BEGIN)), 1)
        self.assertGreaterEqual(len(_frames_of_type(serial, proto.Type.FRAME_DATA)), 1)
        end_frames = _frames_of_type(serial, proto.Type.FRAME_END)
        self.assertEqual(len(end_frames), 1)
        _crc, refresh = struct.unpack_from("<IB", end_frames[0].payload, 0)
        self.assertEqual(refresh, 2)  # FULL_REFRESH_MODE
        self.assertEqual(_draw_bitmap_rects(serial), [])
        # w*h*bpp/8 for the bundled 240x80x1bpp config.
        self.assertEqual(stats.bytes_sent, render_mod.stride_for(240, 1) * 80)


class DirtyRectTests(unittest.TestCase):
    def test_second_frame_from_a_changed_clock_digit_is_a_small_dirty_rect(self):
        device, serials, _clock = connected_device()
        loop = _make_loop(device)
        serial = serials["serial"]

        loop.run_once()  # cycle 0: full frame, seeds _prev_levels
        serial.written_frames.clear()

        stats = loop.run_once()  # cycle 1: only the seconds digit(s) changed

        self.assertFalse(stats.full)
        self.assertGreaterEqual(stats.rects, 1)
        # No full-frame machinery this cycle.
        self.assertEqual(_frames_of_type(serial, proto.Type.FRAME_BEGIN), [])
        rects = _draw_bitmap_rects(serial)
        self.assertGreaterEqual(len(rects), 1)
        full_frame_bytes = render_mod.stride_for(240, 1) * 80
        # The changed region is one digit's glyph, nowhere near the whole
        # panel -- this is the actual point of dirty-rect diffing.
        self.assertLess(stats.bytes_sent, full_frame_bytes // 4)
        for _x, _y, w, h, bpp, op, _n in rects:
            self.assertEqual(bpp, 1)
            self.assertEqual(op, 0)
            self.assertLess(w * h, 240 * 80 // 4)
        # Every DRAW_BITMAP is followed by an explicit PARTIAL_REFRESH for
        # exactly that rect (docs/protocol.md §7.1's wire order: draw into
        # the back buffer first, then refresh -- not the other way around).
        refreshes = _partial_refresh_rects(serial)
        self.assertEqual(len(refreshes), len(rects))
        for (x, y, w, h, _bpp, _op, _n), refresh_rect in zip(rects, refreshes):
            self.assertEqual(refresh_rect, (x, y, w, h))

    def test_dirty_rect_is_page_snapped_to_8_rows(self):
        device, serials, _clock = connected_device()
        loop = _make_loop(device)
        serial = serials["serial"]

        loop.run_once()
        serial.written_frames.clear()
        loop.run_once()

        rects = _draw_bitmap_rects(serial)
        self.assertGreaterEqual(len(rects), 1)
        for _x, y, _w, h, _bpp, _op, _n in rects:
            self.assertEqual(y % 8, 0, f"y={y} is not page-aligned")
            self.assertEqual(h % 8, 0, f"h={h} is not a whole number of 8-row pages")

    def test_identical_frame_sends_nothing(self):
        # A fixed `now_fn` (no ticking) -- nothing at all should change
        # between cycle 0 and cycle 1's *content*; only cycle 0 is forced
        # full anyway (first frame), so make cycle 1 the one under test by
        # disabling periodic full refresh and using a constant clock.
        fixed = datetime.datetime(2026, 9, 3, 14, 32, 7)
        device, serials, _clock = connected_device()
        loop = _make_loop(device, now_fn=lambda: fixed, full_every=0)
        serial = serials["serial"]

        loop.run_once()
        serial.written_frames.clear()
        stats = loop.run_once()

        self.assertFalse(stats.full)
        self.assertEqual(stats.rects, 0)
        self.assertEqual(stats.bytes_sent, 0)
        self.assertEqual(serial.written_frames, [])


class PeriodicFullRefreshTests(unittest.TestCase):
    def test_full_refresh_recurs_every_full_every_frames(self):
        device, serials, _clock = connected_device()
        loop = _make_loop(device, full_every=3)
        serial = serials["serial"]

        fulls = []
        for _ in range(6):
            serial.written_frames.clear()
            stats = loop.run_once()
            fulls.append(stats.full)

        self.assertEqual(fulls, [True, False, False, True, False, False])

    def test_full_every_zero_disables_periodic_full_refresh(self):
        device, serials, _clock = connected_device()
        loop = _make_loop(device, full_every=0)

        fulls = [loop.run_once().full for _ in range(5)]
        self.assertEqual(fulls, [True, False, False, False, False])


class ReconnectTests(unittest.TestCase):
    def test_link_error_triggers_reconnect_and_a_forced_full_retry(self):
        attempts = {"n": 0}

        def factory(_device_path):
            attempts["n"] += 1
            if attempts["n"] == 1:
                # First connection: DRAW_BITMAP (what a dirty-rect cycle
                # sends) never gets answered -- models the link dying
                # partway through a partial-frame cycle.
                return RecordingSerial(silent_types={proto.Type.DRAW_BITMAP})
            return RecordingSerial()  # reconnect gets a fully responsive port

        device, serials, _clock = connected_device(serial_factory=factory)
        loop = _make_loop(device)

        stats0 = loop.run_once()  # full frame -- DRAW_BITMAP never used, succeeds
        self.assertTrue(stats0.full)
        self.assertFalse(stats0.reconnected)

        stats1 = loop.run_once()  # dirty cycle -> DRAW_BITMAP times out -> reconnect
        self.assertTrue(stats1.reconnected)
        self.assertTrue(stats1.full)  # forced full on the post-reconnect retry
        self.assertEqual(attempts["n"], 2)  # exactly one reconnect happened

        # The device is usable afterwards, against the new (second) serial.
        serials["serial"].written_frames.clear()
        stats2 = loop.run_once()
        self.assertFalse(stats2.reconnected)
        self.assertGreaterEqual(stats2.rects, 0)

    def test_reconnect_failure_propagates(self):
        # First connection: DRAW_BITMAP dies (triggers reconnect). Every
        # connection after that (i.e. every reconnect attempt): HELLO
        # itself never answers, so connect_with_backoff can't get a link
        # back at all and, once its attempt budget is spent, must raise.
        attempts = {"n": 0}

        def factory(_device_path):
            attempts["n"] += 1
            if attempts["n"] == 1:
                return RecordingSerial(silent_types={proto.Type.DRAW_BITMAP})
            return RecordingSerial(silent_types={proto.Type.HELLO})

        device, _serials, _clock = connected_device(
            serial_factory=factory,
            reconnect_backoff_s=(0.001,),
        )
        loop = _make_loop(device, reconnect_max_attempts=2)

        loop.run_once()  # full frame succeeds
        with self.assertRaises(TransportError):
            loop.run_once()  # dirty cycle fails; every reconnect attempt also fails HELLO
        # Initial connect (1) + 2 reconnect attempts.
        self.assertEqual(attempts["n"], 3)


class NackReceivedRecoveryTests(unittest.TestCase):
    """run_once() surviving a genuine (non-E_SEQ_GAP) NACK reaching this
    far -- transport.request() already absorbs E_SEQ_GAP itself (see its
    own tests in test_transport.py); this covers the defense-in-depth
    catch run_once() added alongside its existing TransportError handling
    (incident 2026-09-04: before this, device.py's NackReceived was a
    bare Exception and this cycle's `except TransportError` didn't catch
    it, so it propagated straight out of run_once() -- and, via run(),
    out of the whole dashboard process)."""

    def test_nack_received_triggers_reconnect_and_a_forced_full_retry(self):
        attempts = {"n": 0}

        def nacking_draw_bitmap(frame):
            payload = struct.pack("<BBH", proto.ErrorCode.E_STATE, 0, frame.seq)
            return proto.Type.NACK, payload

        def factory(_device_path):
            attempts["n"] += 1
            if attempts["n"] == 1:
                # First connection: DRAW_BITMAP (what a dirty-rect cycle
                # sends) gets a genuine NACK -- models a device confused
                # about its own frame-transaction state, not a dropped
                # link (that's ReconnectTests, above).
                return RecordingSerial(type_replies={proto.Type.DRAW_BITMAP: nacking_draw_bitmap})
            return RecordingSerial()  # reconnect gets a fully responsive port

        device, serials, _clock = connected_device(serial_factory=factory)
        loop = _make_loop(device)

        stats0 = loop.run_once()  # full frame -- DRAW_BITMAP never used, succeeds
        self.assertTrue(stats0.full)
        self.assertFalse(stats0.reconnected)

        stats1 = loop.run_once()  # dirty cycle -> DRAW_BITMAP NACKed -> reconnect
        self.assertTrue(stats1.reconnected)
        self.assertTrue(stats1.full)  # forced full on the post-reconnect retry
        self.assertEqual(attempts["n"], 2)  # exactly one reconnect happened

        # The loop (and the device) are usable afterwards, against the new
        # (second) serial.
        serials["serial"].written_frames.clear()
        stats2 = loop.run_once()
        self.assertFalse(stats2.reconnected)


class OversizedRectFallbackTests(unittest.TestCase):
    """`_send_dirty` has two paths: `DRAW_BITMAP` for a rect small enough
    for one frame (everything at 240x80x1bpp -- covered by DirtyRectTests
    above), and the `FRAME_BEGIN(partial)/FRAME_DATA/FRAME_END` streaming
    fallback for anything bigger (only reachable at 2bpp, or a very large
    bounding box). Exercised directly against `_send_dirty` with synthetic
    levels so it doesn't depend on coaxing a real render into changing
    (almost) the entire 2bpp canvas at once."""

    def test_rect_too_big_for_draw_bitmap_uses_frame_streaming(self):
        device, serials, _clock = connected_device()
        loop = _make_loop(device)
        loop.bpp = 2  # device's caps (hello_ack_payload default) allow it
        serial = serials["serial"]

        w, h = loop.width, loop.height
        prev = [0] * (w * h)
        curr = [3] * (w * h)  # every pixel differs -> whole-canvas dirty rect
        # 240*80 at 2bpp packs to 4800 B, over the 4086 B DRAW_BITMAP budget.
        self.assertGreater(render_mod.stride_for(w, 2) * h, 4086)

        # What choose_dirty_plan (the same function _send_dirty calls) picks
        # for this prev/curr pair -- everything differs, so it's the whole
        # page-snapped canvas as a single rect.
        expected_rects, _is_bounding = render_mod.choose_dirty_plan(prev, curr, w, h, bpp=2, page=loop.page_rows)
        self.assertEqual(len(expected_rects), 1)
        expected = expected_rects[0]

        serial.written_frames.clear()
        rect_count, bytes_sent = loop._send_dirty(prev, curr, w, h)

        self.assertEqual(rect_count, 1)
        self.assertEqual(bytes_sent, render_mod.stride_for(w, 2) * h)
        self.assertEqual(_draw_bitmap_rects(serial), [])  # not this path
        begin_frames = _frames_of_type(serial, proto.Type.FRAME_BEGIN)
        self.assertEqual(len(begin_frames), 1)
        bw, bh, bpp, flags, origin_x = struct.unpack_from("<HHBBH", begin_frames[0].payload, 0)
        self.assertEqual((bw, bh, bpp), (expected.w, expected.h, 2))
        self.assertTrue(flags & 0x01)  # partial-frame bit set
        self.assertEqual(origin_x, expected.x)
        end_frames = _frames_of_type(serial, proto.Type.FRAME_END)
        self.assertEqual(len(end_frames), 1)
        _crc, refresh = struct.unpack_from("<IB", end_frames[0].payload, 0)
        self.assertEqual(refresh, 1)  # PARTIAL_REFRESH_MODE
        # v1 quirk (docs/protocol.md §6.3 "v1 note"): origin_y rides on a
        # PARTIAL_REFRESH sent immediately before FRAME_BEGIN.
        pr = _partial_refresh_rects(serial)
        self.assertEqual(len(pr), 1)
        self.assertEqual(pr[0], (expected.x, expected.y, expected.w, expected.h))


class RunLoopTests(unittest.TestCase):
    def test_once_runs_exactly_one_cycle_and_closes_the_device(self):
        device, serials, _clock = connected_device()
        loop = _make_loop(device)

        loop.run(once=True)

        self.assertEqual(loop._cycle, 1)
        self.assertFalse(serials["serial"].is_open)

    def test_max_cycles_bounds_a_multi_cycle_run(self):
        device, _serials, clock = connected_device()
        # Same ManualClock drives both `clock` (what run() measures elapsed
        # time against) and `sleep` (what it calls to wait) -- deterministic
        # even though the wall-clock-aligned run() now reads self.clock()
        # itself instead of always sleeping a flat `interval` (see
        # docs/troubleshooting.md §6): since
        # run_once() never advances this fake clock, each cycle's target
        # tick is still exactly `interval` away with nothing eaten by
        # render cost, same as the old flat-sleep behavior asserted below.
        loop = _make_loop(device, clock=clock.time, sleep=clock.sleep)

        loop.run(max_cycles=3)

        self.assertEqual(loop._cycle, 3)
        # Slept between cycles (2 sleeps for 3 cycles), not after the last one.
        self.assertEqual(clock.sleep_calls.count(1.0), 2)

    def test_run_is_wall_clock_aligned_not_flat_sleep_after_work(self):
        """The bug fixed by docs/troubleshooting.md §6:
        `run()` used to sleep a flat `interval` *after* `run_once()`
        finished, so real frame period was `interval + run_once() cost`,
        drifting further behind every cycle. Now it targets
        `next_tick += interval` from a fixed start, so cycles land on
        `interval`-spaced wall-clock ticks even though each `run_once()`
        costs real (simulated) time -- proven here with a clock that
        advances by a fixed amount on every read, standing in for
        per-cycle render cost."""
        device, _serials, _clock = connected_device()

        class SteppingClock:
            """`time()` advances by `step` on every call -- standing in for
            each `run_once()` costing a fixed slice of real wall-clock time
            (two of its three calls per cycle land inside `run_once()`
            itself: `t0` and the `render_ms` readback). `sleep(s)` advances
            by exactly `s`, like a real blocking sleep would."""

            def __init__(self, step: float):
                self.t = 0.0
                self.step = step
                self.sleep_calls = []

            def time(self) -> float:
                self.t += self.step
                return self.t

            def sleep(self, seconds: float) -> None:
                self.sleep_calls.append(seconds)
                self.t += seconds

        clock = SteppingClock(step=0.05)  # 50ms/call, well under interval
        loop = _make_loop(device, clock=clock.time, sleep=clock.sleep)

        loop.run(max_cycles=4)

        self.assertEqual(loop._cycle, 4)
        sleeps = clock.sleep_calls
        self.assertEqual(len(sleeps), 3)  # between cycles, not after the last
        # Flat-sleep-after-work (the old bug) would sleep exactly `interval`
        # (1.0) every time regardless of clock reads. Wall-clock alignment
        # must instead shrink each sleep by however much `self.clock()`
        # reads advanced since the previous tick target -- i.e. strictly
        # less than a flat 1.0s, and (since each cycle costs the same fixed
        # slice here) identical cycle to cycle rather than drifting.
        for s in sleeps:
            self.assertGreater(s, 0.0)
            self.assertLess(s, 1.0)
        self.assertAlmostEqual(sleeps[0], sleeps[1], places=9)
        self.assertAlmostEqual(sleeps[1], sleeps[2], places=9)

    def test_run_resyncs_and_warns_after_an_overrun_cycle(self):
        """If a cycle runs long enough to blow past one or more upcoming
        ticks, `run()` must not fire a burst of back-to-back catch-up
        cycles -- it logs the overrun and resyncs to the next tick still in
        the future."""
        device, _serials, _clock = connected_device()

        class JumpingClock:
            """Reads: 0, 0, 3.5, 3.5, 3.5, ... -- i.e. the *second* call
            (right after the first cycle's run_once()) jumps forward by
            3.5 real seconds, well past several 1.0s ticks."""

            def __init__(self):
                self.calls = 0

            def time(self) -> float:
                self.calls += 1
                return 3.5 if self.calls >= 2 else 0.0

        clock = JumpingClock()
        sleeps = []
        loop = _make_loop(device, clock=clock.time, sleep=sleeps.append)

        with self.assertLogs("byok.dashboard.loop", level="WARNING") as cm:
            loop.run(max_cycles=2)

        self.assertEqual(loop._cycle, 2)
        self.assertTrue(any("overran" in m for m in cm.output))
        # Resynced to a tick still ahead of "now" (3.5) -- not a burst of
        # zero/negative sleeps trying to catch up on every missed tick.
        self.assertEqual(len(sleeps), 1)
        self.assertGreaterEqual(sleeps[0], 0.0)


class LoopLockTests(unittest.TestCase):
    """DashboardLoop.run() holding byok.notify_ipc's loop lock for its
    duration -- see notify_ipc.write_lock's own docstring and run()'s.
    Always a tempdir path here, never the real default -- see
    byok.notify_ipc.DEFAULT_LOCK_PATH -- so this never touches
    ~/.cache/byok."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.lock_path = os.path.join(self._tmp.name, "loop.lock")

    def test_lock_cleared_after_a_clean_run(self):
        device, _serials, _clock = connected_device()
        loop = _make_loop(device, lock_path=self.lock_path)

        self.assertFalse(os.path.exists(self.lock_path))
        loop.run(once=True)
        self.assertFalse(os.path.exists(self.lock_path))  # cleared on the way out

    def test_lock_names_this_process_and_is_visible_mid_run(self):
        from byok import notify_ipc

        seen = {}
        device, _serials, _clock = connected_device()
        loop = _make_loop(device, lock_path=self.lock_path)
        real_run_once = loop.run_once

        def spying_run_once():
            # Snapshot the lock file's content while the loop is actually
            # running (run() writes it before entering its cycle loop).
            seen["lock"] = notify_ipc.read_lock(self.lock_path)
            return real_run_once()

        loop.run_once = spying_run_once
        loop.run(once=True)

        self.assertIsNotNone(seen["lock"])
        self.assertEqual(seen["lock"].pid, os.getpid())

    def test_lock_path_none_disables_locking_entirely(self):
        device, _serials, _clock = connected_device()
        loop = _make_loop(device, lock_path=None)
        loop.run(once=True)  # must not raise
        self.assertFalse(os.path.exists(self.lock_path))  # never created


if __name__ == "__main__":
    unittest.main()
