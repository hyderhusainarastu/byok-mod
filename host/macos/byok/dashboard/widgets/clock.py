"""clock widget -- big current time, e.g. "14:32"."""

from __future__ import annotations

from PIL import Image, ImageDraw

from ..fonts import ellipsize, fit_font_size, text_size
from .base import RenderContext, Widget, register


@register("clock")
class ClockWidget(Widget):
    """options:
        format: strftime format, default "%H:%M" (or "%I:%M %p" for 12h)
        title: optional small label drawn above the time
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        fmt = ctx.options.get("format", "%H:%M")
        title = ctx.options.get("title", "")
        text = ctx.now.strftime(fmt)

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        top = self.draw_title(draw, ctx, title) if title else 0

        avail_w = max(1, ctx.width - 4)
        avail_h = max(1, ctx.height - top - 2)
        font = fit_font_size(draw, text, ctx.fonts or _fallback_fonts(ctx), avail_w, avail_h,
                              min_size=8, max_size=max(9, ctx.height), path=ctx.options.get("font"))
        text = ellipsize(draw, text, font, avail_w)
        w, h = text_size(draw, text, font)
        x = (ctx.width - w) // 2
        y = top + (avail_h - h) // 2
        draw.text((max(0, x), max(top, y)), text, font=font, fill=0)
        self.draw_border(image, ctx.options)
        return image


def _fallback_fonts(ctx: RenderContext):
    from ..fonts import FontSet

    return ctx.fonts or FontSet()
