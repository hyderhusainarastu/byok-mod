"""Known-answer tests for byok.render: packing and dithering on tiny inputs.

All expected values in this file are derived by hand from
docs/protocol.md §7.2 (packing) and the documented dithering formulas in
byok/render.py's docstrings, not copied from a captured run of the code.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "host", "macos"))

from byok import render  # noqa: E402


class PackFramebufferTests(unittest.TestCase):
    def test_pack_1bpp_alternating_with_padding(self):
        # 10 pixels, alternating dark/light: level 0 (dark) -> bit 1, level 1
        # (light) -> bit 0. MSB = leftmost, row padded to a whole byte.
        # bits:      1 0 1 0 1 0 1 0 | 1 0 [padded with six 0 bits]
        # byte 0 = 0b10101010 = 0xAA
        # byte 1 = 0b10000000 = 0x80
        levels = [0, 1, 0, 1, 0, 1, 0, 1, 0, 1]
        packed = render.pack_framebuffer(levels, width=10, height=1, bpp=1)
        self.assertEqual(packed, b"\xAA\x80")

    def test_pack_1bpp_all_dark_two_rows(self):
        # width=3 -> stride = ceil(3/8) = 1 byte/row. All dark (level 0) ->
        # all bits 1, top 3 bits set, rest padding zero: 0b11100000 = 0xE0.
        levels = [0, 0, 0] + [0, 0, 0]
        packed = render.pack_framebuffer(levels, width=3, height=2, bpp=1)
        self.assertEqual(packed, b"\xE0\xE0")

    def test_pack_2bpp_exact_byte(self):
        # width=4 exactly fills one byte (4 pixels * 2 bits). Levels 0..3
        # packed MSB-pair-first: 00 01 10 11 = 0b00011011 = 0x1B.
        levels = [0, 1, 2, 3]
        packed = render.pack_framebuffer(levels, width=4, height=1, bpp=2)
        self.assertEqual(packed, b"\x1B")

    def test_pack_2bpp_padding(self):
        # width=3 -> stride = ceil(3*2/8) = 1 byte. Three level-3 pixels then
        # two bits of zero padding: 11 11 11 00 = 0b11111100 = 0xFC.
        levels = [3, 3, 3]
        packed = render.pack_framebuffer(levels, width=3, height=1, bpp=2)
        self.assertEqual(packed, b"\xFC")

    def test_pack_rejects_bad_bpp(self):
        with self.assertRaises(ValueError):
            render.pack_framebuffer([0], 1, 1, bpp=4)

    def test_stride_for(self):
        self.assertEqual(render.stride_for(10, 1), 2)
        self.assertEqual(render.stride_for(8, 1), 1)
        self.assertEqual(render.stride_for(3, 2), 1)
        self.assertEqual(render.stride_for(4, 2), 1)
        self.assertEqual(render.stride_for(5, 2), 2)


class QuantizeNoneTests(unittest.TestCase):
    def test_two_levels_threshold(self):
        # levels=2 -> step=255. round(v/255): 0->0, 64->0, 127->0, 128->1,
        # 191->1, 255->1.
        pixels = [0, 64, 127, 128, 191, 255]
        levels = render.quantize_none(pixels, width=6, height=1, levels=2)
        self.assertEqual(levels, [0, 0, 0, 1, 1, 1])

    def test_four_levels_exact_steps(self):
        # levels=4 -> step=85. Exact multiples map exactly.
        pixels = [0, 85, 170, 255]
        levels = render.quantize_none(pixels, width=4, height=1, levels=4)
        self.assertEqual(levels, [0, 1, 2, 3])

    def test_rejects_too_few_levels(self):
        with self.assertRaises(ValueError):
            render.quantize_none([0], 1, 1, levels=1)


class QuantizeBayerTests(unittest.TestCase):
    def test_flat_mid_gray_reproduces_bayer_checkerboard(self):
        # A flat 128-value 4x4 image, levels=2. frac = 128/255 = 0.50196...
        # Hand-evaluating threshold=(bayer[y][x]+0.5)/16 against that frac
        # for the standard 4x4 Bayer matrix
        #   [[ 0, 8, 2,10],
        #    [12, 4,14, 6],
        #    [ 3,11, 1, 9],
        #    [15, 7,13, 5]]
        # gives level=1 wherever bayer < 8, level=0 wherever bayer >= 8 —
        # i.e. a clean checkerboard.
        pixels = [128] * 16
        levels = render.quantize_bayer(pixels, width=4, height=4, levels=2)
        expected = [
            1, 0, 1, 0,
            0, 1, 0, 1,
            1, 0, 1, 0,
            0, 1, 0, 1,
        ]
        self.assertEqual(levels, expected)

    def test_pure_black_and_white_are_dither_invariant(self):
        pixels = [0] * 16 + [255] * 16
        levels = render.quantize_bayer(pixels, width=4, height=8, levels=2)
        self.assertEqual(levels[:16], [0] * 16)
        self.assertEqual(levels[16:], [1] * 16)


class QuantizeFloydSteinbergTests(unittest.TestCase):
    def test_single_pixel_no_propagation(self):
        # No neighbours to propagate error to: behaves like plain rounding.
        self.assertEqual(render.quantize_floyd_steinberg([50], 1, 1, levels=2), [0])
        self.assertEqual(render.quantize_floyd_steinberg([200], 1, 1, levels=2), [1])

    def test_two_pixel_row_error_propagation(self):
        # levels=2, step=255. x=0: old=128, round(128/255)=round(0.50196)=1
        # (just over the 0.5 boundary) -> level 1, err = 128-255 = -127,
        # propagated in full (only right neighbour exists) to x=1:
        # work[1] = 128 + (-127 * 7/16) = 128 - 55.5625 = 72.4375.
        # x=1: round(72.4375/255) = round(0.284...) = 0 -> level 0.
        levels = render.quantize_floyd_steinberg([128, 128], 2, 1, levels=2)
        self.assertEqual(levels, [1, 0])

    def test_zero_error_pixels_do_not_perturb_neighbours(self):
        # Exact-level pixels (0 and 255 at levels=2) have zero quantization
        # error, so they must not disturb a flat run after them.
        levels = render.quantize_floyd_steinberg([0, 0, 0, 0], 4, 1, levels=2)
        self.assertEqual(levels, [0, 0, 0, 0])
        levels = render.quantize_floyd_steinberg([255, 255, 255, 255], 4, 1, levels=2)
        self.assertEqual(levels, [1, 1, 1, 1])


class DirtyRectTests(unittest.TestCase):
    def test_no_previous_frame_is_fully_dirty(self):
        rect = render.diff_dirty_rect(None, [0, 0, 0, 0], width=2, height=2)
        self.assertEqual(rect, render.DirtyRect(0, 0, 2, 2))

    def test_identical_frames_are_not_dirty(self):
        levels = [1, 0, 1, 0]
        rect = render.diff_dirty_rect(levels, list(levels), width=2, height=2)
        self.assertIsNone(rect)

    def test_single_pixel_change_bounding_box(self):
        prev = [0, 0, 0, 0, 0, 0, 0, 0, 0]  # 3x3, all zero
        curr = list(prev)
        curr[4] = 1  # center pixel, (x=1, y=1)
        rect = render.diff_dirty_rect(prev, curr, width=3, height=3)
        self.assertEqual(rect, render.DirtyRect(1, 1, 1, 1))

    def test_two_corner_changes_give_bounding_box_not_two_rects(self):
        prev = [0] * 9  # 3x3
        curr = list(prev)
        curr[0] = 1  # (0,0)
        curr[8] = 1  # (2,2)
        rect = render.diff_dirty_rect(prev, curr, width=3, height=3)
        self.assertEqual(rect, render.DirtyRect(0, 0, 3, 3))

    def test_length_mismatch_raises(self):
        with self.assertRaises(ValueError):
            render.diff_dirty_rect([0, 0], [0, 0, 0], width=1, height=3)


class CropLevelsTests(unittest.TestCase):
    def test_crop_extracts_subwindow_row_major(self):
        # 4x3, values = row*10 + col, so the result is easy to eyeball.
        levels = [r * 10 + c for r in range(3) for c in range(4)]
        rect = render.DirtyRect(x=1, y=1, w=2, h=2)
        cropped = render.crop_levels(levels, width=4, rect=rect)
        self.assertEqual(cropped, [11, 12, 21, 22])

    def test_crop_full_frame_is_identity(self):
        levels = list(range(12))
        rect = render.DirtyRect(x=0, y=0, w=4, h=3)
        self.assertEqual(render.crop_levels(levels, width=4, rect=rect), levels)


class EstimateI2cTransactionsTests(unittest.TestCase):
    def test_formula_is_overhead_plus_packed_bytes(self):
        # 1bpp, width=10 -> stride = ceil(10/8) = 2 B/row; h=8 -> 16 B.
        rect = render.DirtyRect(x=0, y=0, w=10, h=8)
        self.assertEqual(render.estimate_i2c_transactions(rect, bpp=1), 12 + 16)
        # 2bpp: stride = ceil(10*2/8) = 3 B/row * 8 rows = 24 B.
        self.assertEqual(render.estimate_i2c_transactions(rect, bpp=2), 12 + 24)

    def test_window_overhead_is_overridable(self):
        rect = render.DirtyRect(x=0, y=0, w=8, h=8)
        self.assertEqual(render.estimate_i2c_transactions(rect, bpp=1, window_overhead=0), 8)


class DiffDirtyRectsPagedTests(unittest.TestCase):
    def test_no_previous_frame_is_one_full_canvas_rect(self):
        rects = render.diff_dirty_rects_paged(None, [0] * 16, width=4, height=4, page=8)
        self.assertEqual(rects, [render.DirtyRect(0, 0, 4, 4)])

    def test_identical_frames_yield_no_rects(self):
        levels = [1, 0, 1, 0] * 4
        rects = render.diff_dirty_rects_paged(levels, list(levels), width=4, height=4, page=8)
        self.assertEqual(rects, [])

    def test_one_rect_per_changed_page_band_column_exact(self):
        width, height, page = 64, 16, 8  # two 8-row pages
        prev = [0] * (width * height)
        curr = list(prev)
        curr[0 * width + 0] = 1  # band 0 (rows 0-7): col 0
        curr[8 * width + 63] = 1  # band 1 (rows 8-15): col 63
        rects = render.diff_dirty_rects_paged(prev, curr, width, height, page=page)
        self.assertEqual(
            rects,
            [render.DirtyRect(x=0, y=0, w=1, h=8), render.DirtyRect(x=63, y=8, w=1, h=8)],
        )

    def test_unchanged_band_between_two_changed_bands_is_omitted(self):
        width, height, page = 8, 24, 8  # three 8-row pages; only band 0 and 2 change
        prev = [0] * (width * height)
        curr = list(prev)
        curr[0] = 1  # band 0
        curr[16 * width + 0] = 1  # band 2
        rects = render.diff_dirty_rects_paged(prev, curr, width, height, page=page)
        self.assertEqual(
            rects,
            [render.DirtyRect(x=0, y=0, w=1, h=8), render.DirtyRect(x=0, y=16, w=1, h=8)],
        )


class ChooseDirtyPlanTests(unittest.TestCase):
    def test_nothing_changed_returns_empty_plan(self):
        levels = [0] * 32
        rects, is_bounding = render.choose_dirty_plan(levels, list(levels), width=8, height=4, bpp=1)
        self.assertEqual(rects, [])
        self.assertFalse(is_bounding)

    def test_far_apart_narrow_changes_favor_per_page_rects(self):
        # Two single-pixel changes at opposite ends of a wide canvas, each
        # in its own page band: a single bounding rect would have to span
        # the full width for both bands (128 B), while two page-band rects
        # cost 8 B each -- paged should win on the 12-per-window formula.
        width, height, page = 64, 16, 8
        prev = [0] * (width * height)
        curr = list(prev)
        curr[0 * width + 0] = 1
        curr[8 * width + 63] = 1

        rects, is_bounding = render.choose_dirty_plan(prev, curr, width, height, bpp=1, page=page)

        self.assertFalse(is_bounding)
        self.assertEqual(
            rects,
            [render.DirtyRect(x=0, y=0, w=1, h=8), render.DirtyRect(x=63, y=8, w=1, h=8)],
        )

    def test_many_scattered_bands_favor_one_bounding_rect(self):
        # The same single column (x=0) changes in every one of 8 page
        # bands: 8 separate rects pay the 12-transaction window overhead 8
        # times over, while one page-snapped bounding rect (also just 1
        # column wide here) pays it once -- bounding should win.
        width, height, page = 8, 64, 8
        prev = [0] * (width * height)
        curr = list(prev)
        for band in range(8):
            curr[band * page * width + 0] = 1

        rects, is_bounding = render.choose_dirty_plan(prev, curr, width, height, bpp=1, page=page)

        self.assertTrue(is_bounding)
        self.assertEqual(rects, [render.DirtyRect(x=0, y=0, w=1, h=64)])

    def test_bounding_rect_is_page_snapped(self):
        # A single off-page-boundary change must still come back with y/h
        # snapped to whole 8-row pages when the bounding rect is chosen.
        width, height, page = 8, 16, 8
        prev = [0] * (width * height)
        curr = list(prev)
        curr[5 * width + 0] = 1  # row 5: inside band [0, 8), not page-aligned itself
        rects, _is_bounding = render.choose_dirty_plan(prev, curr, width, height, bpp=1, page=page)
        self.assertEqual(len(rects), 1)
        self.assertEqual(rects[0].y % page, 0)
        self.assertEqual(rects[0].h % page, 0)


class FitToDisplayTests(unittest.TestCase):
    def test_letterbox_preserves_aspect_and_size(self):
        from PIL import Image

        src = Image.new("L", (100, 50), color=0)  # 2:1 landscape, all black
        fitted = render.fit_to_display(src, width=40, height=40, bg=255)
        self.assertEqual(fitted.size, (40, 40))
        # scale = min(40/100, 40/50) = 0.4 -> scaled to 40x20, centered
        # vertically with a 10px light bar top and bottom.
        self.assertEqual(fitted.getpixel((20, 0)), 255)  # letterbox bar
        self.assertEqual(fitted.getpixel((20, 20)), 0)  # inside the image


class RenderImageIntegrationTests(unittest.TestCase):
    def test_render_image_1bpp_roundtrip_shape(self):
        from PIL import Image

        src = Image.new("L", (8, 8), color=255)
        packed, levels = render.render_image(src, width=8, height=8, bpp=1, method="none")
        self.assertEqual(len(levels), 64)
        self.assertEqual(len(packed), render.stride_for(8, 1) * 8)
        # All-white source -> all level 1 (lightest) -> all bits 0.
        self.assertEqual(packed, b"\x00" * 8)

    def test_render_image_2bpp_all_black(self):
        from PIL import Image

        src = Image.new("L", (4, 1), color=0)
        packed, levels = render.render_image(src, width=4, height=1, bpp=2, method="none")
        self.assertEqual(levels, [0, 0, 0, 0])
        self.assertEqual(packed, b"\x00")


if __name__ == "__main__":
    unittest.main()
