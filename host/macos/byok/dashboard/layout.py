"""Grid layout compositor.

Turns a `config.DashboardConfig` + concrete pixel size into a single PIL
'L' (8-bit grayscale) image: each widget's `(at, span)` grid cell is
converted to a pixel rectangle as a *fraction* of the total canvas
(`col / cols`, `row / rows`), so the same config renders sensibly at any
display resolution -- 240x160, 320x240, whatever a future panel needs --
without editing pixel coordinates by hand.

A widget that fails to resolve (unknown `type`) or raises out of
`render()` (a bug, not the I/O failures widgets already guard themselves)
gets a small "[error]" placeholder tile instead of taking down the whole
composite -- one broken widget should never blank the dashboard.
"""

from __future__ import annotations

import logging
from typing import Any, Dict, Optional

from PIL import Image, ImageDraw

from .config import DashboardConfig, WidgetConfig
from .fonts import FontSet
from .widgets.base import RenderContext, WIDGET_REGISTRY

logger = logging.getLogger(__name__)


def widget_rect(w: WidgetConfig, cols: int, rows: int, width: int, height: int):
    """Pixel `(x0, y0, x1, y1)` for a widget's `(at, span)` grid cell, as a
    fraction of `(width, height)` -- this is what makes one config file
    render sensibly at any display resolution."""
    col, row = w.at
    span_w, span_h = w.span
    x0 = round(width * col / cols)
    x1 = round(width * (col + span_w) / cols)
    y0 = round(height * row / rows)
    y1 = round(height * (row + span_h) / rows)
    return max(0, x0), max(0, y0), max(x0 + 1, x1), max(y0 + 1, y1)


# Backwards/internal-compat alias.
_cell_rect = widget_rect


def iter_widget_rects(config: DashboardConfig, width: int, height: int):
    """Yield `(WidgetConfig, (x0, y0, x1, y1))` for every widget, without
    rendering anything -- used by both this module and `render.py`."""
    for w in config.widgets:
        yield w, widget_rect(w, config.grid.cols, config.grid.rows, width, height)


def _error_tile(w: int, h: int, message: str) -> Image.Image:
    w, h = max(1, w), max(1, h)
    img = Image.new("L", (w, h), color=255)
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, 0, w - 1, h - 1], outline=128)
    draw.line([0, 0, w - 1, h - 1], fill=128)
    draw.line([0, h - 1, w - 1, 0], fill=128)
    if w > 20 and h > 10:
        draw.text((2, 2), message[: max(1, (w - 4) // 6)], fill=64)
    return img


def render_dashboard(
    config: DashboardConfig,
    width: int,
    height: int,
    now,
    providers: Optional[Dict[str, Any]] = None,
    fonts: Optional[FontSet] = None,
) -> Image.Image:
    """Compose every widget in `config` onto a `(width, height)` 'L' image."""
    if width <= 0 or height <= 0:
        raise ValueError(f"invalid display size {width}x{height}")

    canvas = Image.new("L", (width, height), color=255)
    fonts = fonts or FontSet(ttf_path=config.fonts.get("path"))
    providers = providers or {}

    for w in config.widgets:
        x0, y0, x1, y1 = _cell_rect(w, config.grid.cols, config.grid.rows, width, height)
        tile_w, tile_h = x1 - x0, y1 - y0

        cls = WIDGET_REGISTRY.get(w.type)
        if cls is None:
            tile = _error_tile(tile_w, tile_h, f"unknown: {w.type}")
        else:
            ctx = RenderContext(
                width=tile_w, height=tile_h, now=now, options=w.options, fonts=fonts, providers=providers
            )
            try:
                tile = cls().render(ctx)
                if tile.size != (tile_w, tile_h):
                    tile = tile.resize((max(1, tile_w), max(1, tile_h)))
                if tile.mode != "L":
                    tile = tile.convert("L")
            except Exception:
                logger.exception("widget %r failed to render", w.type)
                tile = _error_tile(tile_w, tile_h, "[error]")

        canvas.paste(tile, (x0, y0))

    return canvas
