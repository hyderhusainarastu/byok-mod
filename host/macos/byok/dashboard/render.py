"""Compose a dashboard config onto a display-sized, bit-depth-quantized PIL
image.

Two different quantization strategies are applied *per widget tile*, not
uniformly over the whole canvas:

  * widgets whose content is naturally photographic (currently just the
    `image` widget) get ordered (Bayer 4x4) dithering, which trades a bit
    of low-frequency structure for perceived tonal range on a 1bpp/2bpp
    panel -- worthwhile for a photo, unwanted on a line of text.
  * every other widget (clock, date, text, calendar, stats, git, network,
    now playing, shell output, QR codes) gets *no* dithering: plain
    nearest-level thresholding. Dithering turns crisp glyph/module edges
    into speckle, which makes text and QR codes *harder* to read on a
    1bpp panel, not easier, so it is deliberately skipped there.

The result is an 'L' image whose pixel values are already exactly the
levels the target bit depth can represent (0/255 for 1bpp; 0/85/170/255
for 2bpp) -- what `preview.py` writes to PNG is pixel-for-pixel what the
panel will show. The actual on-wire packing (grouping those levels into
1bpp/2bpp framebuffer bytes per docs/protocol.md §7.2) is a separate,
lossless step performed by `byok.render.quantize_levels` /
`pack_framebuffer` in the sibling `byok.render` module -- this package
stays device-protocol agnostic and only ever hands back a plain PIL image.
"""

from __future__ import annotations

import math
from typing import Optional

from PIL import Image

from .config import DashboardConfig
from .fonts import FontSet
from .layout import iter_widget_rects, render_dashboard

# Widget types whose tiles get ordered dithering instead of flat thresholding.
DITHERED_WIDGET_TYPES = {"image"}

_BAYER4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)
_BAYER_N = 4
_BAYER_DENOM = float(_BAYER_N * _BAYER_N)


def _level_to_gray(level: int, levels: int) -> int:
    return round(level * 255.0 / (levels - 1))


def _quantize_none(tile: Image.Image, levels: int) -> Image.Image:
    """Nearest-level thresholding, no dithering -- used for text/graphics."""
    step = 255.0 / (levels - 1)
    lut = []
    for v in range(256):
        level = max(0, min(levels - 1, round(v / step)))
        lut.append(_level_to_gray(level, levels))
    return tile.point(lut)


def _quantize_bayer(tile: Image.Image, levels: int) -> Image.Image:
    """Ordered (Bayer 4x4) dithering -- used for photographic tiles."""
    w, h = tile.size
    src = tile.load()
    out = Image.new("L", (w, h))
    dst = out.load()
    for y in range(h):
        bayer_row = _BAYER4[y % _BAYER_N]
        for x in range(w):
            v = src[x, y]
            scaled = v / 255.0 * (levels - 1)
            base = math.floor(scaled)
            frac = scaled - base
            threshold = (bayer_row[x % _BAYER_N] + 0.5) / _BAYER_DENOM
            level = base + 1 if frac > threshold else base
            level = max(0, min(levels - 1, int(level)))
            dst[x, y] = _level_to_gray(level, levels)
    return out


def render_and_quantize(
    config: DashboardConfig,
    width: int,
    height: int,
    bpp: int,
    now,
    providers: Optional[dict] = None,
    fonts: Optional[FontSet] = None,
) -> Image.Image:
    """Compose `config` at `(width, height)` and quantize to `bpp` bits per
    pixel, dithering only the tiles that want it. Returns a plain 'L' image
    (byte-packing for the wire is `byok.render`'s job, not this module's)."""
    if bpp not in (1, 2):
        raise ValueError(f"bpp must be 1 or 2, got {bpp}")
    levels = 2 ** bpp

    composite = render_dashboard(config, width, height, now, providers=providers, fonts=fonts)
    result = composite.copy()

    for w, (x0, y0, x1, y1) in iter_widget_rects(config, width, height):
        tile = composite.crop((x0, y0, x1, y1))
        if w.type in DITHERED_WIDGET_TYPES:
            quantized = _quantize_bayer(tile, levels)
        else:
            quantized = _quantize_none(tile, levels)
        result.paste(quantized, (x0, y0))

    # Anything not covered by a widget (gaps in the grid) still needs to end
    # up at a legal level for the target bit depth; threshold those areas
    # too rather than leaving unquantized gray behind.
    from PIL import ImageDraw

    covered = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(covered)
    for _w, (x0, y0, x1, y1) in iter_widget_rects(config, width, height):
        draw.rectangle([x0, y0, x1 - 1, y1 - 1], fill=255)
    background = _quantize_none(composite, levels)
    result = Image.composite(result, background, covered)

    return result
