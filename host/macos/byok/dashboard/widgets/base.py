"""Base widget interface.

A widget is a small, pure-ish object: `Widget.render(ctx) -> PIL 'L' image`
of exactly `(ctx.width, ctx.height)` pixels. Widgets must never raise out of
`render()` for missing/failed external data (network, AppleScript, shell) --
catch it and draw a fallback ("--", "N/A", an error line) instead, so one
flaky widget never blanks the whole dashboard.

`Provider` objects (in the per-widget modules) are the one place I/O
happens; they're injected via `ctx.providers` so tests can swap in fakes.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, Optional

from PIL import Image, ImageDraw

from ..fonts import FontSet


@dataclass
class RenderContext:
    """Everything a widget needs to draw itself, and nothing about layout."""

    width: int
    height: int
    now: Any  # datetime.datetime; kept as Any to avoid importing datetime here
    options: Dict[str, Any] = field(default_factory=dict)
    fonts: Optional[FontSet] = None
    providers: Dict[str, Any] = field(default_factory=dict)

    def font(self, size: int, path: Optional[str] = None):
        fs = self.fonts or FontSet()
        return fs.get(size, path=path or self.options.get("font"))


class Widget:
    """Base class for all dashboard widgets. Subclasses set `type_name` and
    implement `render`."""

    type_name = "base"

    def __init__(self, options: Optional[Dict[str, Any]] = None):
        self.options = options or {}

    def render(self, ctx: RenderContext) -> Image.Image:
        raise NotImplementedError

    # -- helpers shared by most widgets -----------------------------------

    def blank_tile(self, ctx: RenderContext, bg: int = 255) -> Image.Image:
        return Image.new("L", (max(1, ctx.width), max(1, ctx.height)), color=bg)

    def draw_border(self, image: Image.Image, options: Dict[str, Any]) -> None:
        if not options.get("border"):
            return
        draw = ImageDraw.Draw(image)
        w, h = image.size
        draw.rectangle([0, 0, w - 1, h - 1], outline=0)

    def draw_title(self, draw: ImageDraw.ImageDraw, ctx: RenderContext, title: str, pad: int = 2) -> int:
        """Draws a small bold-ish title at the top-left; returns the y offset
        content should start at.

        Applies `ctx`'s resolved `text_case` (upper by default on rows
        shorter than 12px -- see `byok.dashboard.fonts.apply_text_case` /
        docs/troubleshooting.md) so every widget's
        title gets that legibility default for free, not just the widgets
        that separately opted into it for their own body text.
        """
        if not title:
            return pad
        from ..fonts import apply_text_case, ellipsize, text_size

        title = apply_text_case(title, ctx)
        font = ctx.font(max(9, ctx.height // 8))
        title = ellipsize(draw, title, font, max(1, ctx.width - 2 * pad))
        draw.text((pad, pad), title, font=font, fill=0)

        _, h = text_size(draw, title, font)
        return pad + h + pad

    def draw_bar(
        self,
        draw: ImageDraw.ImageDraw,
        x: int,
        y: int,
        w: int,
        h: int,
        frac: Optional[float],
    ) -> None:
        """Draws a small horizontal meter: an outlined box `w`x`h` at
        `(x, y)`, filled from the left by `frac` (0.0-1.0, clamped;
        `None` treated as 0 -- an unknown value draws an empty box rather
        than a half-guess). Shared by any "percent toward something"
        widget (e.g. `writing.py`'s daily word-count goal) so they all
        read as the same visual language rather than each inventing its
        own bar. Needs at
        least `h=3` (1px outline top+bottom + 1px fill) to draw anything
        legible; smaller than that, degrades to just the outline.
        """
        if w < 2 or h < 1:
            return
        frac = 0.0 if frac is None else max(0.0, min(1.0, frac))
        draw.rectangle([x, y, x + w - 1, y + h - 1], outline=0)
        if h < 3:
            return
        fill_w = int(round((w - 2) * frac))
        if fill_w > 0:
            draw.rectangle([x + 1, y + 1, x + fill_w, y + h - 2], fill=0)


WIDGET_REGISTRY: Dict[str, type] = {}


def register(type_name: str):
    def _decorator(cls):
        cls.type_name = type_name
        WIDGET_REGISTRY[type_name] = cls
        return cls

    return _decorator
