"""tests/host/test_mirror.py -- unittest suite for byok.mirror (Mode 2,
the screen-mirror pipeline).

Runnable:

    python3 -m unittest tests.host.test_mirror -v
    python3 -m unittest discover -s tests/host

Two kinds of doubles are used, matching the rest of tests/host/:

  * A *fake serial port* (`_fake_transport.RecordingSerial` /
    `connected_device`) for anything that goes to a device -- same
    technique test_dashboard_loop.py uses. Never a real serial port.
  * A *fake capture helper* (`_fake_mirror_helper.py`, run as a real
    subprocess -- never the real Swift `ScreenMirrorHelper` binary, and no
    ScreenCaptureKit/screen access of any kind) standing in for
    `ScreenMirrorHelper capture`'s stdout stream of `SMH1` frame records.

No real serial port and no real screen capture are ever touched here.
"""

from __future__ import annotations

import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from byok import proto  # noqa: E402
from byok import render as render_mod  # noqa: E402
from byok import mirror  # noqa: E402

from _fake_transport import RecordingSerial, connected_device, hello_ack_payload  # noqa: E402


_FAKE_HELPER = os.path.join(_THIS_DIR, "_fake_mirror_helper.py")


class ManualClock:
    """Same shape as _fake_transport.ManualClock, kept local so this file
    has no non-obvious cross-test-module coupling for something this
    small."""

    def __init__(self) -> None:
        self.t = 0.0
        self.sleep_calls = []

    def time(self) -> float:
        return self.t

    def sleep(self, seconds: float) -> None:
        self.sleep_calls.append(seconds)
        self.t += seconds


def spawn_fake_helper(*, width=32, height=16, frames=3, pattern="static",
                       block_size=4, permission_fail=False, exit_code=0):
    args = [sys.executable, _FAKE_HELPER, "capture",
            "--width", str(width), "--height", str(height),
            "--frames", str(frames), "--pattern", pattern,
            "--block-size", str(block_size), "--exit-code", str(exit_code)]
    if permission_fail:
        args.append("--permission-fail")
    return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def make_device(width=32, height=16, bpp_native=1):
    """(device, serials, clock) -- a real Device over a fake serial port,
    HELLO_ACK reporting `width`x`height` so it matches the fake helper's
    frame size in tests that connect a device."""
    payload = hello_ack_payload(display_w=width, display_h=height, bpp_native=bpp_native)
    return connected_device(hello_payload=payload)


def frames_of_type(serial: RecordingSerial, msg_type: int):
    return [f for f in serial.written_frames if f.type == msg_type]


def draw_bitmap_rects(serial: RecordingSerial):
    out = []
    for f in frames_of_type(serial, proto.Type.DRAW_BITMAP):
        x, y, w, h, bpp, op = struct.unpack_from("<HHHHBB", f.payload, 0)
        out.append((x, y, w, h, bpp, op, len(f.payload) - 10))
    return out


class ReadSmh1FrameTests(unittest.TestCase):
    """Unit tests for the wire-level reader, no subprocess involved."""

    @staticmethod
    def _encode(width, height, pixels, dirty=(0, 0, None, None)):
        dx, dy, dw, dh = dirty
        dw = width if dw is None else dw
        dh = height if dh is None else dh
        return struct.pack(">4sHHHHHH", b"SMH1", width, height, dx, dy, dw, dh) + pixels

    def test_clean_eof_returns_none(self):
        self.assertIsNone(mirror.read_smh1_frame(io.BytesIO(b"")))

    def test_valid_frame_decodes(self):
        pixels = bytes(range(6)) * 1  # 2x3 canvas, 6 bytes
        stream = io.BytesIO(self._encode(2, 3, pixels))
        frame = mirror.read_smh1_frame(stream)
        self.assertEqual((frame.width, frame.height), (2, 3))
        self.assertEqual(frame.pixels, pixels)
        # A second read on the same stream is a clean EOF.
        self.assertIsNone(mirror.read_smh1_frame(stream))

    def test_truncated_header_raises(self):
        stream = io.BytesIO(b"SMH1\x00\x02")  # 6 of 16 header bytes
        with self.assertRaises(mirror.HelperError):
            mirror.read_smh1_frame(stream)

    def test_truncated_pixels_raises(self):
        header = struct.pack(">4sHHHHHH", b"SMH1", 4, 4, 0, 0, 4, 4)
        stream = io.BytesIO(header + b"\x00" * 5)  # needs 16 pixel bytes, has 5
        with self.assertRaises(mirror.HelperError):
            mirror.read_smh1_frame(stream)

    def test_bad_magic_raises(self):
        header = struct.pack(">4sHHHHHH", b"XXXX", 2, 2, 0, 0, 2, 2)
        stream = io.BytesIO(header + b"\x00" * 4)
        with self.assertRaises(mirror.HelperError):
            mirror.read_smh1_frame(stream)

    def test_multiple_frames_read_in_sequence(self):
        stream = io.BytesIO(
            self._encode(1, 1, b"\x00") + self._encode(1, 1, b"\xff")
        )
        f1 = mirror.read_smh1_frame(stream)
        f2 = mirror.read_smh1_frame(stream)
        self.assertEqual(f1.pixels, b"\x00")
        self.assertEqual(f2.pixels, b"\xff")
        self.assertIsNone(mirror.read_smh1_frame(stream))


class RateLimiterTests(unittest.TestCase):
    def test_first_call_never_waits(self):
        clock = ManualClock()
        rl = mirror.RateLimiter(2.0, clock=clock.time, sleep=clock.sleep)
        rl.wait()
        self.assertEqual(clock.sleep_calls, [])

    def test_spacing_is_enforced_at_1_over_fps(self):
        clock = ManualClock()
        rl = mirror.RateLimiter(2.0, clock=clock.time, sleep=clock.sleep)  # 0.5s interval
        rl.wait()  # no wait, seeds _last at t=0
        rl.wait()  # elapsed=0 -> sleep 0.5
        rl.wait()  # elapsed=0 (clock only moves via sleep) -> sleep 0.5
        self.assertEqual(clock.sleep_calls, [0.5, 0.5])

    def test_no_wait_when_enough_time_already_elapsed(self):
        clock = ManualClock()
        rl = mirror.RateLimiter(2.0, clock=clock.time, sleep=clock.sleep)
        rl.wait()
        clock.t += 1.0  # a whole second passed doing other work
        rl.wait()
        self.assertEqual(clock.sleep_calls, [])

    def test_fps_zero_never_waits(self):
        clock = ManualClock()
        rl = mirror.RateLimiter(0.0, clock=clock.time, sleep=clock.sleep)
        rl.wait()
        rl.wait()
        self.assertEqual(clock.sleep_calls, [])


class RegionCropTests(unittest.TestCase):
    """`_canvas_image` always returns the pipeline's target size, whether
    or not --region is set."""

    def _pipeline(self, **kwargs):
        clock = ManualClock()
        kwargs.setdefault("device", None)
        kwargs.setdefault("stream", io.BytesIO(b""))
        kwargs.setdefault("width", 32)
        kwargs.setdefault("height", 16)
        kwargs.setdefault("bpp", 1)
        kwargs.setdefault("dry_run", True)
        kwargs.setdefault("clock", clock.time)
        kwargs.setdefault("sleep", clock.sleep)
        return mirror.MirrorPipeline(**kwargs)

    def test_no_region_passes_canvas_through_unchanged(self):
        pipe = self._pipeline()
        pixels = bytes((i * 7) % 256 for i in range(32 * 16))
        frame = mirror.MirrorFrame(32, 16, 0, 0, 32, 16, pixels)
        img = pipe._canvas_image(frame)
        self.assertEqual(img.size, (32, 16))
        self.assertEqual(img.tobytes(), pixels)

    def test_region_crops_and_refits_to_target_size(self):
        pipe = self._pipeline(region=(4, 4, 8, 8))
        pixels = bytes([0xFF] * (32 * 16))
        frame = mirror.MirrorFrame(32, 16, 0, 0, 32, 16, pixels)
        img = pipe._canvas_image(frame)
        # Cropping+re-fitting always lands back on the pipeline's own
        # target size, regardless of the requested region's size.
        self.assertEqual(img.size, (32, 16))

    def test_region_clamped_to_frame_bounds(self):
        pipe = self._pipeline(region=(30, 14, 100, 100))  # far past the 32x16 frame
        pixels = bytes([0x00] * (32 * 16))
        frame = mirror.MirrorFrame(32, 16, 0, 0, 32, 16, pixels)
        img = pipe._canvas_image(frame)  # must not raise, must stay target-sized
        self.assertEqual(img.size, (32, 16))


class SubprocessPipelineTests(unittest.TestCase):
    """Integration tests driving MirrorPipeline against a real
    subprocess -- the fake helper script -- and (where a device is
    involved) a real Device over a fake serial port. No real screen
    capture, no real serial port."""

    def _run(self, proc, **kwargs):
        clock = ManualClock()
        kwargs.setdefault("clock", clock.time)
        kwargs.setdefault("sleep", clock.sleep)
        kwargs.setdefault("stream", proc.stdout)
        kwargs.setdefault("process", proc)
        pipe = mirror.MirrorPipeline(**kwargs)
        self.addCleanup(mirror.terminate_process, proc)
        return pipe, clock

    def test_first_frame_is_a_full_frame_transaction(self):
        device, serials, _clock = make_device(width=32, height=16)
        proc = spawn_fake_helper(width=32, height=16, frames=1, pattern="static")
        pipe, _ = self._run(proc, device=device, width=32, height=16, bpp=1, dither="none")

        stats = pipe.run_once()

        self.assertTrue(stats.full)
        self.assertEqual(stats.rects, 1)
        serial = serials["serial"]
        self.assertEqual(len(frames_of_type(serial, proto.Type.FRAME_BEGIN)), 1)
        end_frames = frames_of_type(serial, proto.Type.FRAME_END)
        self.assertEqual(len(end_frames), 1)
        _crc, refresh = struct.unpack_from("<IB", end_frames[0].payload, 0)
        self.assertEqual(refresh, 2)  # FULL_REFRESH_MODE
        full_frame_bytes = render_mod.stride_for(32, 1) * 16
        self.assertEqual(stats.dirty_bytes, full_frame_bytes)

    def test_moving_block_produces_a_bounded_dirty_rect(self):
        device, serials, _clock = make_device(width=32, height=16)
        proc = spawn_fake_helper(width=32, height=16, frames=2, pattern="moving-block", block_size=4)
        pipe, _ = self._run(
            proc, device=device, width=32, height=16, bpp=1, dither="none", full_every=0,
        )
        serial = serials["serial"]

        pipe.run_once()  # cycle 0: full frame, seeds _prev_levels
        serial.written_frames.clear()
        stats = pipe.run_once()  # cycle 1: block moved -> small dirty region

        self.assertFalse(stats.full)
        self.assertGreaterEqual(stats.rects, 1)
        full_frame_bytes = render_mod.stride_for(32, 1) * 16  # 64 B
        self.assertLess(stats.dirty_bytes, full_frame_bytes // 2)
        self.assertEqual(frames_of_type(serial, proto.Type.FRAME_BEGIN), [])  # DRAW_BITMAP path
        rects = draw_bitmap_rects(serial)
        self.assertGreaterEqual(len(rects), 1)
        for _x, _y, w, h, bpp, op, _n in rects:
            self.assertEqual(bpp, 1)
            self.assertEqual(op, 0)
            self.assertLess(w * h, 32 * 16 // 2)  # bounded, nowhere near the whole canvas

    def test_identical_frames_send_nothing(self):
        device, serials, _clock = make_device(width=32, height=16)
        proc = spawn_fake_helper(width=32, height=16, frames=2, pattern="static")
        pipe, _ = self._run(
            proc, device=device, width=32, height=16, bpp=1, dither="none", full_every=0,
        )
        serial = serials["serial"]

        pipe.run_once()
        serial.written_frames.clear()
        stats = pipe.run_once()

        self.assertFalse(stats.full)
        self.assertEqual(stats.rects, 0)
        self.assertEqual(stats.dirty_bytes, 0)
        self.assertEqual(serial.written_frames, [])

    def test_fps_limiter_spaces_out_sends(self):
        proc = spawn_fake_helper(width=32, height=16, frames=3, pattern="static")
        pipe, clock = self._run(
            proc, device=None, dry_run=True, width=32, height=16, bpp=1,
            dither="none", fps=2.0, print_stats=False,
        )

        pipe.run_once()  # no wait (first call)
        pipe.run_once()  # waits 0.5s
        pipe.run_once()  # waits 0.5s

        self.assertEqual(clock.sleep_calls, [0.5, 0.5])

    def test_dry_run_prints_per_frame_stats_with_no_device(self):
        proc = spawn_fake_helper(width=32, height=16, frames=2, pattern="moving-block", block_size=4)
        pipe, _ = self._run(
            proc, device=None, dry_run=True, width=32, height=16, bpp=1,
            dither="none", full_every=0,
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            pipe.run_once()
            pipe.run_once()
        out = buf.getvalue().splitlines()

        self.assertEqual(len(out), 2)
        self.assertIn("frame 0:", out[0])
        self.assertIn("full", out[0])
        self.assertIn("frame 1:", out[1])
        self.assertIn("dirty_bytes=", out[1])
        self.assertIn("est_i2c_txns=", out[1])

    def test_dry_run_preview_writes_numbered_pngs(self):
        from PIL import Image

        proc = spawn_fake_helper(width=32, height=16, frames=2, pattern="moving-block", block_size=4)
        with tempfile.TemporaryDirectory() as tmp:
            preview_dir = os.path.join(tmp, "preview")
            pipe, _ = self._run(
                proc, device=None, dry_run=True, width=32, height=16, bpp=1,
                dither="none", full_every=0, preview_path=preview_dir, print_stats=False,
            )
            pipe.run_once()
            pipe.run_once()

            frame0 = os.path.join(preview_dir, "frame-000000.png")
            frame1 = os.path.join(preview_dir, "frame-000001.png")
            self.assertTrue(os.path.isfile(frame0))
            self.assertTrue(os.path.isfile(frame1))
            with Image.open(frame0) as img:
                self.assertEqual(img.size, (32, 16))

    def test_live_preview_overwrites_a_single_file(self):
        from PIL import Image

        device, _serials, _clock = make_device(width=32, height=16)
        proc = spawn_fake_helper(width=32, height=16, frames=1, pattern="static")
        with tempfile.TemporaryDirectory() as tmp:
            preview_path = os.path.join(tmp, "preview.png")
            pipe, _ = self._run(
                proc, device=device, width=32, height=16, bpp=1, dither="none",
                preview_path=preview_path,
            )
            pipe.run_once()
            self.assertTrue(os.path.isfile(preview_path))
            with Image.open(preview_path) as img:
                self.assertEqual(img.size, (32, 16))

    def test_permission_failure_raises_a_clear_error(self):
        proc = spawn_fake_helper(width=32, height=16, permission_fail=True)
        pipe, _ = self._run(
            proc, device=None, dry_run=True, width=32, height=16, bpp=1, dither="none",
        )

        with self.assertRaises(mirror.HelperPermissionError) as ctx:
            pipe.run()
        message = str(ctx.exception)
        self.assertIn("Screen Recording", message)

    def test_nonzero_non_permission_exit_raises_helper_exited(self):
        proc = spawn_fake_helper(width=32, height=16, frames=1, pattern="static", exit_code=7)
        pipe, _ = self._run(
            proc, device=None, dry_run=True, width=32, height=16, bpp=1, dither="none",
            print_stats=False,
        )

        with self.assertRaises(mirror.HelperExited):
            pipe.run()

    def test_clean_exit_after_frames_returns_normally(self):
        proc = spawn_fake_helper(width=32, height=16, frames=2, pattern="static")
        pipe, _ = self._run(
            proc, device=None, dry_run=True, width=32, height=16, bpp=1, dither="none",
            print_stats=False,
        )
        # A clean (exit 0) helper close after all frames are consumed is
        # not an error -- run() just stops. Only a non-zero exit (a crash)
        # or exit 3 (permission) raise.
        pipe.run()  # must not raise
        self.assertEqual(pipe._cycle, 2)


class BandRectGeometryTests(unittest.TestCase):
    """`compute_band_rect` -- the pure "crop to cover" geometry `--fit
    fill`/`band` use to pick a panel-aspect-ratio strip out of a larger
    capture, in place of the helper's own letterbox-preserving scale-to-fit
    (docs/sample-projects/mirror-and-virtual-display.md §4)."""

    def test_square_capture_middle_band_is_the_centre_third(self):
        # A 240x240 capture cropped to a 240x80 (3:1) target: three equal
        # 240x80 bands stack exactly inside the square, and "middle" is
        # the centre one -- the concrete case that fixed the 2026-09-03
        # on-device "small square in the panel centre" report.
        self.assertEqual(mirror.compute_band_rect(240, 240, 240, 80, "middle"), (0, 80, 240, 80))

    def test_square_capture_top_and_bottom_bands(self):
        self.assertEqual(mirror.compute_band_rect(240, 240, 240, 80, "top"), (0, 0, 240, 80))
        self.assertEqual(mirror.compute_band_rect(240, 240, 240, 80, "bottom"), (0, 160, 240, 80))

    def test_scaled_up_square_capture_scales_the_same_band_proportionally(self):
        # --scale 3 requests a 720x720 capture instead of 240x240; the
        # selected band scales with it, still centred, still exactly
        # panel-aspect-ratio (720:240 == 3:1 == 240:80).
        self.assertEqual(mirror.compute_band_rect(720, 720, 240, 80, "middle"), (0, 240, 720, 240))

    def test_source_wider_than_target_crops_width_centered(self):
        # A very wide source (e.g. an ultrawide display) relative to the
        # 3:1 panel: width is cropped instead of height, and always
        # centered regardless of `band` (there is no top/bottom concept
        # on the cropped axis here).
        rect = mirror.compute_band_rect(1000, 100, 240, 80, "middle")
        self.assertEqual(rect, (350, 0, 300, 100))
        self.assertEqual(rect, mirror.compute_band_rect(1000, 100, 240, 80, "top"))
        self.assertEqual(rect, mirror.compute_band_rect(1000, 100, 240, 80, "bottom"))

    def test_exact_aspect_match_crops_nothing(self):
        self.assertEqual(mirror.compute_band_rect(480, 160, 240, 80, "middle"), (0, 0, 480, 160))

    def test_degenerate_zero_sized_input_does_not_raise(self):
        self.assertEqual(mirror.compute_band_rect(0, 0, 240, 80), (0, 0, 1, 1))
        self.assertEqual(mirror.compute_band_rect(240, 80, 0, 0), (0, 0, 240, 80))


class ThresholdTests(unittest.TestCase):
    """`otsu_threshold`/`mean_threshold` -- the auto-threshold cut points
    `--dither none` uses under `--fit fill`/`band` instead of the fixed
    127.5 midpoint `render.quantize_none` applies unconditionally."""

    def test_otsu_clean_bimodal_histogram(self):
        pixels = [0] * 50 + [255] * 50
        t = mirror.otsu_threshold(pixels)
        # `<=` classification (see run_once) must still separate the two
        # clusters cleanly regardless of where exactly the tie lands.
        levels = [0 if p <= t else 1 for p in pixels]
        self.assertEqual(set(levels[:50]), {0})
        self.assertEqual(set(levels[50:]), {1})

    def test_otsu_off_centre_contrast_beats_a_fixed_midpoint(self):
        # Dark background (30) with bright text (220) -- far from the
        # naive 127.5 midpoint on both sides, the realistic case a fixed
        # threshold gets wrong.
        pixels = [30] * 80 + [220] * 20
        t = mirror.otsu_threshold(pixels)
        self.assertTrue(30 <= t < 220)
        levels = [0 if p <= t else 1 for p in pixels]
        self.assertEqual(levels.count(0), 80)
        self.assertEqual(levels.count(1), 20)

    def test_otsu_empty_and_uniform_fall_back_to_128(self):
        self.assertEqual(mirror.otsu_threshold([]), 128)
        self.assertEqual(mirror.otsu_threshold([64] * 10), 128)

    def test_mean_threshold_basic(self):
        self.assertEqual(mirror.mean_threshold([0, 100]), 50)
        self.assertEqual(mirror.mean_threshold([]), 128)

    def test_mean_luminance(self):
        self.assertEqual(mirror.mean_luminance([0, 100]), 50.0)
        self.assertEqual(mirror.mean_luminance([]), 0.0)


class InvertHysteresisTests(unittest.TestCase):
    """`decide_invert` -- per-frame auto-invert decision with hysteresis
    so a mean luminance hovering near the boundary doesn't flip every
    cycle (docs/sample-projects/mirror-and-virtual-display.md §4)."""

    def test_on_and_off_are_unconditional(self):
        self.assertTrue(mirror.decide_invert(250.0, False, "on"))
        self.assertFalse(mirror.decide_invert(5.0, True, "off"))

    def test_auto_dark_frame_inverts(self):
        self.assertTrue(mirror.decide_invert(20.0, False, "auto"))

    def test_auto_light_frame_does_not_invert(self):
        self.assertFalse(mirror.decide_invert(230.0, True, "auto"))

    def test_auto_dead_zone_keeps_previous_state(self):
        mid = (mirror.INVERT_ON_BELOW + mirror.INVERT_OFF_ABOVE) / 2
        self.assertFalse(mirror.decide_invert(mid, False, "auto"))
        self.assertTrue(mirror.decide_invert(mid, True, "auto"))

    def test_auto_does_not_flicker_across_a_hovering_sequence(self):
        # A luminance sequence that drifts across the dead zone without
        # ever crossing either hard threshold must never change state.
        state = False
        state = mirror.decide_invert(20.0, state, "auto")
        self.assertTrue(state)
        for lum in (105.0, 120.0, 130.0, 115.0, 100.0):
            state = mirror.decide_invert(lum, state, "auto")
            self.assertTrue(state, f"flipped at lum={lum}")
        state = mirror.decide_invert(200.0, state, "auto")
        self.assertFalse(state)


class ContextualDefaultsTests(unittest.TestCase):
    """`resolve_contextual_defaults` -- --fit/--dither's --window vs
    --display-dependent defaults, and `capture_size` -- what --fit/--scale
    request from the helper."""

    def _parse(self, argv):
        return mirror.build_parser().parse_args(argv)

    def test_window_defaults_to_fill_and_none(self):
        args = self._parse(["--window", "1234"])
        mirror.resolve_contextual_defaults(args)
        self.assertEqual(args.fit, "fill")
        self.assertEqual(args.dither, "none")

    def test_display_defaults_to_contain_and_bayer(self):
        args = self._parse(["--display", "1"])
        mirror.resolve_contextual_defaults(args)
        self.assertEqual(args.fit, "contain")
        self.assertEqual(args.dither, "bayer")

    def test_explicit_fit_and_dither_are_never_overridden(self):
        args = self._parse(["--window", "1234", "--fit", "contain", "--dither", "floyd"])
        mirror.resolve_contextual_defaults(args)
        self.assertEqual(args.fit, "contain")
        self.assertEqual(args.dither, "floyd")

    def test_explicit_band_fit_on_display_keeps_bayer(self):
        # The auto-'none' dither default is gated on --window specifically
        # (per the mirror mode's own spec, "when --fit is fill/band on a
        # window") -- a --display capture explicitly put into --fit band
        # still gets the ordinary --display dither default (bayer), not
        # the window-only text default.
        args = self._parse(["--display", "1", "--fit", "band"])
        mirror.resolve_contextual_defaults(args)
        self.assertEqual(args.fit, "band")
        self.assertEqual(args.dither, "bayer")

    def test_capture_size_contain_matches_panel_aspect_ratio(self):
        self.assertEqual(mirror.capture_size(240, 80, "contain", scale=1), (240, 80))
        self.assertEqual(mirror.capture_size(240, 80, "contain", scale=3), (720, 240))

    def test_capture_size_fill_band_stretch_are_square(self):
        for fit in ("fill", "band", "stretch"):
            self.assertEqual(mirror.capture_size(240, 80, fit, scale=1), (240, 240))
            self.assertEqual(mirror.capture_size(240, 80, fit, scale=3), (720, 720))


class FitAndInvertIntegrationTests(unittest.TestCase):
    """End-to-end (fake-helper subprocess, no device) coverage of --fit
    fill/band cropping and --invert auto against a real MirrorPipeline
    cycle, not just the pure geometry/threshold functions in isolation."""

    @staticmethod
    def _encode_frame(width, height, pixels):
        return struct.pack(">4sHHHHHH", b"SMH1", width, height, 0, 0, width, height) + pixels

    def _pipeline(self, stream, **kwargs):
        clock = ManualClock()
        kwargs.setdefault("device", None)
        kwargs.setdefault("dry_run", True)
        kwargs.setdefault("print_stats", False)
        kwargs.setdefault("width", 24)
        kwargs.setdefault("height", 8)
        kwargs.setdefault("bpp", 1)
        kwargs.setdefault("clock", clock.time)
        kwargs.setdefault("sleep", clock.sleep)
        return mirror.MirrorPipeline(stream=stream, **kwargs)

    def test_fill_crop_picks_the_middle_band_not_the_letterbox(self):
        # A 24x24 square capture: a light "window" fills the middle
        # third (rows 8-16) with a mid-gray value, black (letterbox-like)
        # elsewhere -- --fit fill on a 24x8 target must land squarely on
        # the middle band's content, matching compute_band_rect(24, 24,
        # 24, 8, "middle") == (0, 8, 24, 8).
        canvas = bytearray(b"\x00" * (24 * 24))
        for y in range(8, 16):
            for x in range(24):
                canvas[y * 24 + x] = 0xC0
        stream = io.BytesIO(self._encode_frame(24, 24, bytes(canvas)))
        pipe = self._pipeline(stream, fit="fill", dither="none", invert="off")
        pipe.run_once()
        self.assertEqual(set(pipe._prev_levels), {1})  # all-light band -> all level-1 (off)

    def test_band_top_selects_a_different_strip_than_middle(self):
        canvas = bytearray(b"\x00" * (24 * 24))
        for y in range(0, 8):  # only the top band is light this time
            for x in range(24):
                canvas[y * 24 + x] = 0xC0
        stream = io.BytesIO(self._encode_frame(24, 24, bytes(canvas)))
        pipe_top = self._pipeline(stream, fit="band", band="top", dither="none", invert="off")
        pipe_top.run_once()
        self.assertEqual(set(pipe_top._prev_levels), {1})

        stream2 = io.BytesIO(self._encode_frame(24, 24, bytes(canvas)))
        pipe_middle = self._pipeline(stream2, fit="band", band="middle", dither="none", invert="off")
        pipe_middle.run_once()
        self.assertEqual(set(pipe_middle._prev_levels), {0})  # middle band is still black here

    def test_stretch_ignores_aspect_ratio(self):
        # A non-square, non-panel-aspect-ratio capture: --fit stretch must
        # still land on exactly the target size with no crash/letterbox.
        w, h = 30, 20
        pixels = bytes([0x80] * (w * h))
        stream = io.BytesIO(self._encode_frame(w, h, pixels))
        pipe = self._pipeline(stream, fit="stretch", dither="none", invert="off")
        stats = pipe.run_once()
        self.assertEqual(len(pipe._prev_levels), 24 * 8)
        self.assertTrue(stats.full)

    def test_invert_auto_inverts_a_dark_frame_and_reports_it_in_stats(self):
        # Uniformly dark canvas well below INVERT_ON_BELOW.
        pixels = bytes([20] * (24 * 8))
        stream = io.BytesIO(self._encode_frame(24, 8, pixels))
        pipe = self._pipeline(stream, fit="contain", dither="none", invert="auto")
        stats = pipe.run_once()
        self.assertTrue(stats.invert)
        self.assertAlmostEqual(stats.mean_luminance, 20.0)
        # Inverted, a uniformly dark frame becomes uniformly light -> all
        # pixels quantize to the lightest (off) level.
        self.assertEqual(set(pipe._prev_levels), {1})

    def test_invert_auto_hysteresis_persists_across_cycles(self):
        # cycle 0: clearly dark -> inverts. cycle 1: a mid-gray frame in
        # the hysteresis dead zone must NOT un-invert.
        dark = bytes([20] * (24 * 8))
        mid = bytes([120] * (24 * 8))
        proc_stream = io.BytesIO(self._encode_frame(24, 8, dark) + self._encode_frame(24, 8, mid))
        pipe = self._pipeline(proc_stream, fit="contain", dither="none", invert="auto", full_every=0)

        stats0 = pipe.run_once()
        self.assertTrue(stats0.invert)
        stats1 = pipe.run_once()
        self.assertTrue(stats1.invert, "mid-gray dead-zone frame flipped invert state off")

    def test_threshold_method_otsu_is_used_by_default_for_fill_dither_none(self):
        pixels = bytearray([30] * (24 * 8))
        for i in range(0, 24 * 8, 5):
            pixels[i] = 220
        stream = io.BytesIO(self._encode_frame(24, 8, bytes(pixels)))
        pipe = self._pipeline(stream, fit="fill", dither="none", invert="off")
        stats = pipe.run_once()
        self.assertIsNotNone(stats.threshold)
        self.assertTrue(30 <= stats.threshold < 220)

    def test_threshold_method_fixed_reports_no_auto_threshold(self):
        pixels = bytes([30] * (24 * 8))
        stream = io.BytesIO(self._encode_frame(24, 8, pixels))
        pipe = self._pipeline(
            stream, fit="fill", dither="none", invert="off", threshold_method="fixed",
        )
        stats = pipe.run_once()
        self.assertIsNone(stats.threshold)

    def test_threshold_only_applies_to_fill_and_band_not_contain(self):
        pixels = bytes([30] * (24 * 8))
        stream = io.BytesIO(self._encode_frame(24, 8, pixels))
        pipe = self._pipeline(stream, fit="contain", dither="none", invert="off")
        stats = pipe.run_once()
        self.assertIsNone(stats.threshold)  # contain keeps render.quantize_none's fixed midpoint


class InvalidConstructionTests(unittest.TestCase):
    def _pipeline(self, **overrides):
        clock = ManualClock()
        kwargs = dict(
            device=None, stream=io.BytesIO(b""), width=24, height=8, bpp=1, dry_run=True,
            clock=clock.time, sleep=clock.sleep,
        )
        kwargs.update(overrides)
        return mirror.MirrorPipeline(**kwargs)

    def test_unknown_fit_raises(self):
        with self.assertRaises(ValueError):
            self._pipeline(fit="zoom")

    def test_unknown_band_raises(self):
        with self.assertRaises(ValueError):
            self._pipeline(band="left")

    def test_unknown_invert_raises(self):
        with self.assertRaises(ValueError):
            self._pipeline(invert="maybe")

    def test_unknown_threshold_method_raises(self):
        with self.assertRaises(ValueError):
            self._pipeline(threshold_method="magic")


class ReconnectTests(unittest.TestCase):
    def test_link_error_triggers_reconnect_and_a_forced_full_retry(self):
        attempts = {"n": 0}

        def factory(_device_path):
            attempts["n"] += 1
            if attempts["n"] == 1:
                return RecordingSerial(
                    silent_types={proto.Type.DRAW_BITMAP},
                    hello_payload=hello_ack_payload(display_w=32, display_h=16),
                )
            return RecordingSerial(hello_payload=hello_ack_payload(display_w=32, display_h=16))

        device, serials, clock = connected_device(serial_factory=factory)
        proc = spawn_fake_helper(width=32, height=16, frames=2, pattern="moving-block", block_size=4)
        self.addCleanup(mirror.terminate_process, proc)
        pipe = mirror.MirrorPipeline(
            device=device, stream=proc.stdout, process=proc, width=32, height=16, bpp=1,
            dither="none", full_every=0, clock=clock.time, sleep=clock.sleep,
        )

        stats0 = pipe.run_once()  # full frame: DRAW_BITMAP unused, succeeds
        self.assertTrue(stats0.full)
        self.assertFalse(stats0.reconnected)

        stats1 = pipe.run_once()  # dirty cycle -> DRAW_BITMAP times out -> reconnect -> forced full retry
        self.assertTrue(stats1.reconnected)
        self.assertTrue(stats1.full)
        self.assertEqual(attempts["n"], 2)

    def test_nack_received_triggers_reconnect_and_a_forced_full_retry(self):
        """Same recovery as a dropped link, for a genuine (non-
        E_SEQ_GAP) NACK -- transport.request() already absorbs
        E_SEQ_GAP itself (test_transport.py), so a NackReceived reaching
        run_once() is a real protocol-level refusal. Mirrors
        dashboard/loop.py's equivalent test (incident 2026-09-04)."""
        attempts = {"n": 0}

        def nacking_draw_bitmap(frame):
            payload = struct.pack("<BBH", int(proto.ErrorCode.E_STATE), 0, frame.seq)
            return proto.Type.NACK, payload

        def factory(_device_path):
            attempts["n"] += 1
            if attempts["n"] == 1:
                return RecordingSerial(
                    type_replies={proto.Type.DRAW_BITMAP: nacking_draw_bitmap},
                    hello_payload=hello_ack_payload(display_w=32, display_h=16),
                )
            return RecordingSerial(hello_payload=hello_ack_payload(display_w=32, display_h=16))

        device, serials, clock = connected_device(serial_factory=factory)
        proc = spawn_fake_helper(width=32, height=16, frames=2, pattern="moving-block", block_size=4)
        self.addCleanup(mirror.terminate_process, proc)
        pipe = mirror.MirrorPipeline(
            device=device, stream=proc.stdout, process=proc, width=32, height=16, bpp=1,
            dither="none", full_every=0, clock=clock.time, sleep=clock.sleep,
        )

        stats0 = pipe.run_once()  # full frame: DRAW_BITMAP unused, succeeds
        self.assertTrue(stats0.full)
        self.assertFalse(stats0.reconnected)

        stats1 = pipe.run_once()  # dirty cycle -> DRAW_BITMAP NACKed -> reconnect -> forced full retry
        self.assertTrue(stats1.reconnected)
        self.assertTrue(stats1.full)
        self.assertEqual(attempts["n"], 2)


if __name__ == "__main__":
    unittest.main()
