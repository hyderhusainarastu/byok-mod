"""Image -> device framebuffer rendering.

Pipeline: fit_to_display -> to_grayscale -> quantize_levels -> pack_framebuffer.

Pixel packing follows docs/protocol.md §7.2 exactly:

  * 1 bpp: 8 pixels/byte, MSB = leftmost, rows padded to a whole byte,
    ``1`` = pixel on (dark on the FSTN panel).
  * 2 bpp: 4 pixels/byte, most-significant *pair* = leftmost, values ``0``
    (darkest) .. ``3`` (lightest), same row padding/order.

Internally, both cases are represented the same way before packing: a flat,
row-major list of "levels", one per pixel, in ``0 .. (2**bpp - 1)``, where
**level 0 is always darkest and the highest level is always lightest** — this
matches 2 bpp's on-wire meaning directly, and for 1 bpp the packer inverts it
(level 0 -> bit 1) to match that mode's "1 = dark" convention. Quantizers
never need to know which bpp they're feeding.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple

from PIL import Image

Levels = List[int]  # flat, row-major, length w*h, values in [0, levels-1]

# 4x4 Bayer (ordered dither) threshold matrix, standard ordering.
_BAYER4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)
_BAYER4_N = 4
_BAYER4_DENOM = float(_BAYER4_N * _BAYER4_N)


# --------------------------------------------------------------------------
# Fit / grayscale
# --------------------------------------------------------------------------


def to_grayscale(image: Image.Image) -> Image.Image:
    """Convert to 8-bit grayscale ('L' mode). No-op if already 'L'."""
    return image if image.mode == "L" else image.convert("L")


def fit_to_display(image: Image.Image, width: int, height: int, bg: int = 255) -> Image.Image:
    """Scale `image` to fit within (width, height) preserving aspect ratio,
    then letterbox onto a `bg`-filled canvas of exactly (width, height).

    Grayscale in, grayscale out. `bg=255` (white/light) matches the panel's
    light background; pass `bg=0` for a dark letterbox instead.
    """
    if width <= 0 or height <= 0:
        raise ValueError(f"invalid target size {width}x{height}")

    gray = to_grayscale(image)
    src_w, src_h = gray.size
    if src_w <= 0 or src_h <= 0:
        raise ValueError(f"invalid source image size {src_w}x{src_h}")

    scale = min(width / src_w, height / src_h)
    new_w = max(1, round(src_w * scale))
    new_h = max(1, round(src_h * scale))
    resample = Image.LANCZOS if hasattr(Image, "LANCZOS") else Image.BICUBIC
    scaled = gray.resize((new_w, new_h), resample=resample)

    canvas = Image.new("L", (width, height), color=bg)
    off_x = (width - new_w) // 2
    off_y = (height - new_h) // 2
    canvas.paste(scaled, (off_x, off_y))
    return canvas


# --------------------------------------------------------------------------
# Quantization
# --------------------------------------------------------------------------


def _clamp_level(v: int, levels: int) -> int:
    return 0 if v < 0 else (levels - 1 if v > levels - 1 else v)


def quantize_none(pixels: Sequence[int], width: int, height: int, levels: int) -> Levels:
    """Plain nearest-level thresholding, no dithering."""
    if levels < 2:
        raise ValueError("levels must be >= 2")
    step = 255.0 / (levels - 1)
    out = [0] * (width * height)
    for i, v in enumerate(pixels):
        out[i] = _clamp_level(round(v / step), levels)
    return out


def quantize_bayer(pixels: Sequence[int], width: int, height: int, levels: int) -> Levels:
    """Ordered (Bayer 4x4) dithering to `levels` evenly spaced levels."""
    if levels < 2:
        raise ValueError("levels must be >= 2")
    out = [0] * (width * height)
    for y in range(height):
        row_base = y * width
        bayer_row = _BAYER4[y % _BAYER4_N]
        for x in range(width):
            v = pixels[row_base + x]
            scaled = v / 255.0 * (levels - 1)
            base = math.floor(scaled)
            frac = scaled - base
            threshold = (bayer_row[x % _BAYER4_N] + 0.5) / _BAYER4_DENOM
            level = base + 1 if frac > threshold else base
            out[row_base + x] = _clamp_level(int(level), levels)
    return out


def quantize_floyd_steinberg(pixels: Sequence[int], width: int, height: int, levels: int) -> Levels:
    """Floyd-Steinberg error-diffusion dithering to `levels` evenly spaced levels."""
    if levels < 2:
        raise ValueError("levels must be >= 2")
    step = 255.0 / (levels - 1)
    work = [float(v) for v in pixels]
    out = [0] * (width * height)
    for y in range(height):
        row_base = y * width
        for x in range(width):
            i = row_base + x
            old = work[i]
            if old < 0.0:
                old = 0.0
            elif old > 255.0:
                old = 255.0
            level = _clamp_level(round(old / step), levels)
            out[i] = level
            err = old - level * step
            if err == 0.0:
                continue
            if x + 1 < width:
                work[i + 1] += err * (7.0 / 16.0)
            if y + 1 < height:
                if x > 0:
                    work[i + width - 1] += err * (3.0 / 16.0)
                work[i + width] += err * (5.0 / 16.0)
                if x + 1 < width:
                    work[i + width + 1] += err * (1.0 / 16.0)
    return out


_QUANTIZERS = {
    "none": quantize_none,
    "bayer": quantize_bayer,
    "floyd": quantize_floyd_steinberg,
}


def quantize_levels(
    image: Image.Image, levels: int, method: str = "floyd"
) -> Tuple[Levels, int, int]:
    """Quantize a grayscale PIL image to `levels` levels. Returns (levels, w, h)."""
    if method not in _QUANTIZERS:
        raise ValueError(f"unknown dither method {method!r}; choose from {sorted(_QUANTIZERS)}")
    gray = to_grayscale(image)
    w, h = gray.size
    pixels = list(gray.getdata())
    return _QUANTIZERS[method](pixels, w, h, levels), w, h


# --------------------------------------------------------------------------
# Packing (docs/protocol.md §7.2)
# --------------------------------------------------------------------------


def stride_for(width: int, bpp: int) -> int:
    return (width * bpp + 7) // 8


def pack_framebuffer(levels: Levels, width: int, height: int, bpp: int) -> bytes:
    """Pack a flat level array (0=darkest) into the on-wire framebuffer format.

    `bpp` must be 1 or 2. `levels` values must be in `[0, 2**bpp - 1]`.
    """
    if bpp == 1:
        return _pack_1bpp(levels, width, height)
    if bpp == 2:
        return _pack_2bpp(levels, width, height)
    raise ValueError(f"bpp must be 1 or 2, got {bpp}")


def _pack_1bpp(levels: Levels, width: int, height: int) -> bytes:
    stride = stride_for(width, 1)
    out = bytearray(stride * height)
    for y in range(height):
        row_base = y * width
        out_row_base = y * stride
        acc = 0
        nbits = 0
        byte_i = out_row_base
        for x in range(width):
            level = levels[row_base + x]
            bit = 1 if level == 0 else 0  # level 0 (darkest) -> pixel on
            acc = (acc << 1) | bit
            nbits += 1
            if nbits == 8:
                out[byte_i] = acc
                byte_i += 1
                acc = 0
                nbits = 0
        if nbits:
            acc <<= 8 - nbits  # pad remaining low bits with 0, MSB-first
            out[byte_i] = acc
    return bytes(out)


def _pack_2bpp(levels: Levels, width: int, height: int) -> bytes:
    stride = stride_for(width, 2)
    out = bytearray(stride * height)
    for y in range(height):
        row_base = y * width
        out_row_base = y * stride
        acc = 0
        npairs_bits = 0  # bits accumulated so far in the current byte
        byte_i = out_row_base
        for x in range(width):
            level = levels[row_base + x] & 0x03
            acc = (acc << 2) | level
            npairs_bits += 2
            if npairs_bits == 8:
                out[byte_i] = acc
                byte_i += 1
                acc = 0
                npairs_bits = 0
        if npairs_bits:
            acc <<= 8 - npairs_bits
            out[byte_i] = acc
    return bytes(out)


def render_image(
    image: Image.Image,
    width: int,
    height: int,
    bpp: int,
    method: str = "floyd",
    bg: int = 255,
) -> Tuple[bytes, Levels]:
    """Full pipeline: fit -> grayscale -> quantize -> pack.

    Returns (packed_bytes, levels) — `levels` is handed back so the caller
    can keep it as the "previous frame" for `diff_dirty_rect`.
    """
    if bpp not in (1, 2):
        raise ValueError(f"bpp must be 1 or 2, got {bpp}")
    fitted = fit_to_display(image, width, height, bg=bg)
    levels, w, h = quantize_levels(fitted, levels=2 ** bpp, method=method)
    packed = pack_framebuffer(levels, w, h, bpp)
    return packed, levels


# --------------------------------------------------------------------------
# Dirty-rect diffing
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class DirtyRect:
    x: int
    y: int
    w: int
    h: int


def diff_dirty_rect(
    prev: Optional[Levels], curr: Levels, width: int, height: int
) -> Optional[DirtyRect]:
    """Bounding box of every pixel that changed between `prev` and `curr`.

    Returns None if `prev` is None (treat as "first frame, redraw all" is
    the caller's call) — actually returns None only when there is NO change;
    when `prev is None` every pixel is considered changed (full-frame dirty
    rect), matching "nothing has ever been drawn yet".
    """
    if len(curr) != width * height:
        raise ValueError("curr length does not match width*height")
    if prev is None:
        return DirtyRect(0, 0, width, height) if width and height else None
    if len(prev) != len(curr):
        raise ValueError("prev/curr length mismatch")

    min_x, min_y = width, height
    max_x, max_y = -1, -1
    for y in range(height):
        row_base = y * width
        row_changed = False
        for x in range(width):
            i = row_base + x
            if prev[i] != curr[i]:
                row_changed = True
                if x < min_x:
                    min_x = x
                if x > max_x:
                    max_x = x
        if row_changed:
            if y < min_y:
                min_y = y
            if y > max_y:
                max_y = y

    if max_x < 0:
        return None  # identical
    return DirtyRect(x=min_x, y=min_y, w=max_x - min_x + 1, h=max_y - min_y + 1)


def crop_levels(levels: Levels, width: int, rect: DirtyRect) -> Levels:
    """Extract the `rect` sub-window of a flat, row-major `levels` array
    (row stride `width`) as its own flat, row-major array (row stride
    `rect.w`) -- what a partial-window packing call needs."""
    out: Levels = []
    for yy in range(rect.y, rect.y + rect.h):
        row_start = yy * width + rect.x
        out.extend(levels[row_start: row_start + rect.w])
    return out


def _snap_y_to_pages(y: int, h: int, height: int, page: int) -> Tuple[int, int]:
    """Expand `[y, y+h)` outward to whole `page`-row bands, clamped to
    `[0, height)`. Column bounds (x/w) are untouched -- the panel controller
    addresses partial refreshes column-exact but row-page-granular (see
    docs/display.md)."""
    y0 = (y // page) * page
    y1 = min(height, -(-(y + h) // page) * page)  # ceil div by `page`
    return y0, y1 - y0


def diff_dirty_rects_paged(
    prev: Optional[Levels], curr: Levels, width: int, height: int, page: int = 8
) -> List[DirtyRect]:
    """Like `diff_dirty_rect`, but instead of one global bounding box,
    returns one `DirtyRect` per changed `page`-row band: `y`/`h` snapped to
    that band exactly, `x`/`w` column-exact (tight to that band's own
    changed columns, not the whole frame's). Bands with no change are
    omitted entirely. Empty list means identical frames.

    `prev is None` (first frame) returns a single full-canvas rect, same
    convention as `diff_dirty_rect`.
    """
    if len(curr) != width * height:
        raise ValueError("curr length does not match width*height")
    if prev is None:
        return [DirtyRect(0, 0, width, height)] if width and height else []
    if len(prev) != len(curr):
        raise ValueError("prev/curr length mismatch")

    rects: List[DirtyRect] = []
    for y0 in range(0, height, page):
        y1 = min(height, y0 + page)
        min_x, max_x = width, -1
        changed = False
        for y in range(y0, y1):
            row_base = y * width
            for x in range(width):
                i = row_base + x
                if prev[i] != curr[i]:
                    changed = True
                    if x < min_x:
                        min_x = x
                    if x > max_x:
                        max_x = x
        if changed:
            rects.append(DirtyRect(x=min_x, y=y0, w=max_x - min_x + 1, h=y1 - y0))
    return rects


def estimate_i2c_transactions(rect: DirtyRect, bpp: int, window_overhead: int = 12) -> int:
    """Rough cost, in I2C transactions, of pushing `rect` to the panel:
    `window_overhead` (default 12 -- programming the controller's column/page
    address window) plus one transaction per packed pixel byte."""
    return window_overhead + stride_for(rect.w, bpp) * rect.h


def choose_dirty_plan(
    prev: Optional[Levels],
    curr: Levels,
    width: int,
    height: int,
    bpp: int,
    page: int = 8,
    window_overhead: int = 12,
) -> Tuple[List[DirtyRect], bool]:
    """Pick the cheaper of two ways to push what changed: one `DirtyRect`
    per changed page-band (`diff_dirty_rects_paged`), or a single page-snapped
    bounding rect over the whole changed area -- whichever costs fewer
    estimated I2C transactions (`estimate_i2c_transactions`), summed across
    every rect actually sent.

    Returns `(rects, is_bounding)`. `rects` is `[]` if `prev == curr`
    (nothing to send). Every returned rect has `y`/`h` already snapped to
    `page`-row bands.
    """
    paged = diff_dirty_rects_paged(prev, curr, width, height, page=page)
    if not paged:
        return [], False

    bbox = diff_dirty_rect(prev, curr, width, height)
    assert bbox is not None  # paged non-empty implies at least one changed pixel
    by0, bh = _snap_y_to_pages(bbox.y, bbox.h, height, page)
    bounding = DirtyRect(x=bbox.x, y=by0, w=bbox.w, h=bh)

    cost_paged = sum(estimate_i2c_transactions(r, bpp, window_overhead) for r in paged)
    cost_bounding = estimate_i2c_transactions(bounding, bpp, window_overhead)

    if cost_bounding <= cost_paged:
        return [bounding], True
    return paged, False
