"""tests/host/test_dashboard_partial_update.py -- regression test for the
partial-update ("dirty rect") path itself.

Motivated by docs/troubleshooting.md §6: the fastest
way that investigation ruled out the dirty-rect/page-snapping/DRAW_BITMAP
packer as the *cause* of the reported garble was proving the full-frame
`preview.py` path (which never touches any of that machinery) reproduced
the identical corruption. That was a negative result for *this* incident,
but it also means partial-update correctness itself had no direct
regression test before this file -- §3.4(e) reads the packer code by eye
and concludes it's correct "by construction", which is exactly the kind of
claim a test should pin down instead of re-deriving from source every time.

This test renders two consecutive real frames (real Pillow font rendering,
real bundled config, no device/transport at all) that differ only by the
clock widget's seconds digit, computes the same dirty plan
`DashboardLoop._send_dirty` would compute, and replays it -- pack, then
unpack, then paste into a simulated device framebuffer seeded with the
*previous* frame -- exactly the sequence real firmware does with a
DRAW_BITMAP payload. The result must equal the full new frame pixel-for-
pixel. This catches any future regression in `crop_levels`/`pack_framebuffer`
row/stride/offset handling or in `choose_dirty_plan`'s page-snapping that a
"read the code" review could miss.

Runnable:

    python3 -m unittest tests.host.test_dashboard_partial_update -v
"""

import datetime
import os
import sys
import unittest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)

from byok import render as render_mod  # noqa: E402
from byok.dashboard import config as config_mod  # noqa: E402
from byok.dashboard import render as dash_render  # noqa: E402
from byok.dashboard.fonts import FontSet  # noqa: E402

_CONFIG_PATH = config_mod.default_config_path()


def _unpack_1bpp(packed: bytes, width: int, height: int) -> render_mod.Levels:
    """Inverse of `byok.render._pack_1bpp` -- decode packed on-wire bytes
    back into a flat, row-major level array (0=darkest, matching
    `render.py`'s "1 = dark" convention: bit 1 -> level 0)."""
    stride = render_mod.stride_for(width, 1)
    out = [0] * (width * height)
    for y in range(height):
        row_base = y * width
        byte_row_base = y * stride
        for x in range(width):
            byte = packed[byte_row_base + (x // 8)]
            bit = (byte >> (7 - (x % 8))) & 1
            out[row_base + x] = 0 if bit else 1
    return out


def _render_levels(now, bpp=1, width=240, height=80):
    cfg = config_mod.load(_CONFIG_PATH)
    fonts = FontSet(ttf_path=cfg.fonts.get("path"))
    composite = dash_render.render_and_quantize(
        cfg, width, height, bpp, now, providers={}, fonts=fonts,
    )
    levels, w, h = render_mod.quantize_levels(composite, levels=2 ** bpp, method="none")
    return levels, w, h


class PartialUpdateFramebufferTests(unittest.TestCase):
    def test_applying_the_dirty_plan_reproduces_the_full_new_frame(self):
        bpp = 1
        now0 = datetime.datetime(2026, 9, 3, 20, 19, 51)
        now1 = now0 + datetime.timedelta(seconds=1)  # changes the clock's seconds digit

        prev, w, h = _render_levels(now0, bpp=bpp)
        curr, w2, h2 = _render_levels(now1, bpp=bpp)
        self.assertEqual((w, h), (w2, h2))
        # Sanity: the two frames actually differ (otherwise this test would
        # trivially pass with an empty dirty plan and prove nothing).
        self.assertNotEqual(prev, curr)

        rects, _is_bounding = render_mod.choose_dirty_plan(prev, curr, w, h, bpp, page=8)
        self.assertGreaterEqual(len(rects), 1)

        # Simulated device framebuffer: starts as whatever the previous
        # frame left it at, exactly like the real panel's back buffer
        # between cycles.
        sim = list(prev)
        for rect in rects:
            self.assertEqual(rect.y % 8, 0)
            self.assertEqual(rect.h % 8, 0)
            cropped = render_mod.crop_levels(curr, w, rect)
            packed = render_mod.pack_framebuffer(cropped, rect.w, rect.h, bpp)
            unpacked = _unpack_1bpp(packed, rect.w, rect.h)
            for yy in range(rect.h):
                src_row = yy * rect.w
                dst_row = (rect.y + yy) * w + rect.x
                sim[dst_row: dst_row + rect.w] = unpacked[src_row: src_row + rect.w]

        self.assertEqual(sim, curr, "dirty-rect replay did not reproduce the full new frame")

    def test_second_dirty_plan_from_a_static_frame_is_empty(self):
        # Companion sanity check: replaying an *empty* dirty plan against an
        # already-matching simulated framebuffer is a no-op that still
        # equals curr (curr == prev here).
        bpp = 1
        now = datetime.datetime(2026, 9, 3, 20, 19, 51)
        prev, w, h = _render_levels(now, bpp=bpp)
        curr, _w2, _h2 = _render_levels(now, bpp=bpp)  # identical timestamp

        rects, _is_bounding = render_mod.choose_dirty_plan(prev, curr, w, h, bpp, page=8)
        self.assertEqual(rects, [])

        sim = list(prev)
        self.assertEqual(sim, curr)


if __name__ == "__main__":
    unittest.main()
