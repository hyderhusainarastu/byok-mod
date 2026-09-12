"""tests/host/test_device_image.py -- `byok image` / `Device.image()`
coverage: file format handling (PBM/PNG/JPEG via Pillow), non-3:1
letterboxing, and the three `--dither` modes.

Runnable:

    python3 -m unittest tests.host.test_device_image -v

Goes through a real `Device` + `SerialTransport` against a fake serial
port (`_fake_transport`, same technique as test_transport.py) -- proves
`Device.image()` (what `cli.py cmd_image` calls) actually accepts each
file format and streams a correctly-shaped frame, not just that
`byok.render`'s pure functions do.
"""

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

from PIL import Image  # noqa: E402

from byok import proto  # noqa: E402
from byok import render  # noqa: E402

from _fake_transport import connected_device  # noqa: E402


def _write_test_image(path: str, size, fmt: str, color=128) -> None:
    img = Image.new("L", size, color=color)
    # A couple of shapes so it isn't a flat field -- exercises the dithers
    # for real rather than trivially thresholding one value everywhere.
    for x in range(0, size[0], 4):
        for y in range(size[1]):
            if (x + y) % 8 < 4:
                img.putpixel((x, y), 255 - color)
    if fmt == "PBM":
        img.convert("1").save(path, format="PPM")
    else:
        img.save(path, format=fmt)


class ImageFileFormatTests(unittest.TestCase):
    """`Device.image()` (the `byok image PATH` command's implementation)
    opens the path with `PIL.Image.open`, so any format Pillow reads is
    supported -- this exercises the three the project explicitly commits
    to (PBM, PNG, JPEG)."""

    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmpdir.cleanup)

    def _run_image_command(self, path):
        device, serials, _clock = connected_device()
        levels = device.image(path)
        return levels, serials["serial"]

    def _assert_full_frame_sent(self, serial, expected_bytes):
        end_frames = [f for f in serial.written_frames if f.type == proto.Type.FRAME_END]
        self.assertEqual(len(end_frames), 1)
        _crc, refresh = struct.unpack_from("<IB", end_frames[0].payload, 0)
        self.assertEqual(refresh, 2)  # FULL_REFRESH_MODE, Device.image()'s default
        data_frames = [f for f in serial.written_frames if f.type == proto.Type.FRAME_DATA]
        total = sum(len(f.payload) - 4 for f in data_frames)
        self.assertEqual(total, expected_bytes)

    def test_pbm_source(self):
        path = os.path.join(self.tmpdir.name, "src.pbm")
        _write_test_image(path, (100, 60), "PBM")
        levels, serial = self._run_image_command(path)
        self.assertEqual(len(levels), 240 * 80)
        self._assert_full_frame_sent(serial, render.stride_for(240, 1) * 80)

    def test_png_source(self):
        path = os.path.join(self.tmpdir.name, "src.png")
        _write_test_image(path, (100, 60), "PNG")
        levels, serial = self._run_image_command(path)
        self.assertEqual(len(levels), 240 * 80)
        self._assert_full_frame_sent(serial, render.stride_for(240, 1) * 80)

    def test_jpeg_source(self):
        path = os.path.join(self.tmpdir.name, "src.jpg")
        _write_test_image(path, (100, 60), "JPEG")
        levels, serial = self._run_image_command(path)
        self.assertEqual(len(levels), 240 * 80)
        self._assert_full_frame_sent(serial, render.stride_for(240, 1) * 80)


class DitherOptionTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmpdir.cleanup)
        self.path = os.path.join(self.tmpdir.name, "gradient.png")
        # A horizontal gradient -- a flat mid-gray field would make "none"
        # vs "bayer" vs "floyd" indistinguishable in the checks below.
        img = Image.new("L", (100, 60))
        for x in range(100):
            for y in range(60):
                img.putpixel((x, y), int(255 * x / 99))
        img.save(self.path, format="PNG")

    def test_each_dither_mode_produces_a_valid_frame(self):
        for method in sorted(render._QUANTIZERS):
            with self.subTest(method=method):
                device, serials, _clock = connected_device()
                levels = device.image(self.path, dither=method)
                self.assertEqual(len(levels), 240 * 80)
                self.assertTrue(all(v in (0, 1) for v in levels))  # 1bpp native
                self._assert_ack(serials["serial"])

    def test_none_vs_floyd_produce_different_output_on_a_gradient(self):
        # Not a claim about which is "better" -- just that --dither is
        # actually wired through to distinct quantizers, not ignored.
        device_a, _s, _c = connected_device()
        levels_none = device_a.image(self.path, dither="none")
        device_b, _s2, _c2 = connected_device()
        levels_floyd = device_b.image(self.path, dither="floyd")
        self.assertNotEqual(levels_none, levels_floyd)

    @staticmethod
    def _assert_ack(serial):
        end_frames = [f for f in serial.written_frames if f.type == proto.Type.FRAME_END]
        assert len(end_frames) == 1


class LetterboxTests(unittest.TestCase):
    """The panel is 3:1 (240x80); anything else must be letterboxed, not
    stretched -- covered at the `fit_to_display` level (pure, easy to
    assert on exactly) and once through the full `Device.image()` path."""

    def test_square_image_letterboxed_left_and_right(self):
        src = Image.new("L", (100, 100), color=0)  # 1:1, all black
        fitted = render.fit_to_display(src, width=240, height=80, bg=255)
        self.assertEqual(fitted.size, (240, 80))
        # scale = min(240/100, 80/100) = 0.8 -> scaled to 80x80 exactly
        # filling the height, centered horizontally: 80px light bars each
        # side (240 - 80) / 2 = 80.
        self.assertEqual(fitted.getpixel((0, 40)), 255)  # left bar
        self.assertEqual(fitted.getpixel((239, 40)), 255)  # right bar
        self.assertEqual(fitted.getpixel((120, 40)), 0)  # inside the image
        self.assertEqual(fitted.getpixel((120, 0)), 0)  # image fills full height
        self.assertEqual(fitted.getpixel((120, 79)), 0)

    def test_tall_image_letterboxed_top_and_bottom(self):
        src = Image.new("L", (50, 100), color=0)  # 1:2 portrait, all black
        fitted = render.fit_to_display(src, width=240, height=80, bg=255)
        # scale = min(240/50, 80/100) = 0.8 -> scaled to 40x80, centered
        # horizontally (200px of light bars total, no vertical bars since
        # the scaled image already fills the full 80px height).
        self.assertEqual(fitted.getpixel((0, 40)), 255)
        self.assertEqual(fitted.getpixel((120, 40)), 0)

    def test_non_3to1_image_via_device_image_is_letterboxed_not_stretched(self):
        path = tempfile.mktemp(suffix=".png", dir=tempfile.gettempdir())
        Image.new("L", (100, 100), color=0).save(path)
        self.addCleanup(lambda: os.remove(path) if os.path.exists(path) else None)

        device, _serials, _clock = connected_device()
        levels = device.image(path, dither="none")

        # Letterbox bars (light, level = 2**bpp - 1 = 1 for 1bpp) down the
        # left/right edges; content (dark, level 0) in the vertical center
        # strip. width=240 -> row stride 240 in the flat `levels` array.
        width = 240
        mid_row = 40 * width
        self.assertEqual(levels[mid_row + 0], 1)  # far-left edge: bar
        self.assertEqual(levels[mid_row + 239], 1)  # far-right edge: bar
        self.assertEqual(levels[mid_row + 120], 0)  # center: image content


if __name__ == "__main__":
    unittest.main()
