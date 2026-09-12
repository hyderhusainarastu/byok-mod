"""text widget -- arbitrary static/config-driven text with word wrap."""

from __future__ import annotations

from typing import List

from PIL import Image, ImageDraw

from ..fonts import apply_text_case, text_size
from .base import RenderContext, Widget, register


def wrap_text(draw, text: str, font, max_width: int) -> List[str]:
    """Greedy word wrap: breaks on whitespace; a single word longer than
    `max_width` is hard-broken by character so it doesn't overflow."""
    lines: List[str] = []
    for paragraph in text.split("\n"):
        words = paragraph.split(" ")
        current = ""
        for word in words:
            candidate = f"{current} {word}".strip()
            if text_size(draw, candidate, font)[0] <= max_width or not current:
                current = candidate
            else:
                lines.append(current)
                current = word
            while text_size(draw, current, font)[0] > max_width and len(current) > 1:
                # hard-break an over-long single word/fragment
                lo, hi = 1, len(current)
                while lo < hi:
                    mid = (lo + hi + 1) // 2
                    if text_size(draw, current[:mid], font)[0] <= max_width:
                        lo = mid
                    else:
                        hi = mid - 1
                lines.append(current[:lo])
                current = current[lo:]
        lines.append(current)
    return lines


@register("text")
class TextWidget(Widget):
    """options:
        text: the string to display (required; empty renders a blank tile)
        title: optional header
        size: font size in px, default derived from tile height
        align: "left" | "center", default "left"
        text_case: "upper" | "as-is", default "upper" for rows shorter than
            12px (see byok.dashboard.fonts.resolve_text_case) -- set
            "as-is" explicitly if this text's exact casing matters (a name,
            a quote) more than default legibility at that size
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        text = apply_text_case(str(ctx.options.get("text", "")), ctx)
        title = ctx.options.get("title", "")
        align = ctx.options.get("align", "left")

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        y = self.draw_title(draw, ctx, title) if title else 2

        size = int(ctx.options.get("size", max(9, ctx.height // 8)))
        font = ctx.font(size)
        max_width = max(1, ctx.width - 4)
        for line in wrap_text(draw, text, font, max_width):
            w, h = text_size(draw, line, font)
            if y + h > ctx.height - 1:
                break  # vertical overflow: remaining lines silently dropped,
                       # same convention as calendar.py/reminders.py's lists
            x = 2 if align != "center" else max(2, (ctx.width - w) // 2)
            draw.text((x, y), line, font=font, fill=0)
            y += h + 1

        self.draw_border(image, ctx.options)
        return image
