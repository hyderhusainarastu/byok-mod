"""date widget -- current date, e.g. "Thu, Sep 3"."""

from __future__ import annotations

from PIL import Image, ImageDraw

from ..fonts import apply_text_case, ellipsize, fit_font_size, text_size
from .base import RenderContext, Widget, register


@register("date")
class DateWidget(Widget):
    """options:
        format: strftime format, default "%a, %b %-d" (falls back to %#d on
                platforms without %-d, then to plain %d)
        text_case: "upper" | "as-is", default "upper" for rows shorter than
            12px (see byok.dashboard.fonts.resolve_text_case) -- most
            formats include letters ("Thu", "Sep"), which are exactly the
            thin-lowercase-stroke case that default protects against
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        fmt = ctx.options.get("format", "%a, %b %-d")
        text = apply_text_case(self._strftime(ctx.now, fmt), ctx)

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        avail_w = max(1, ctx.width - 4)
        avail_h = max(1, ctx.height - 2)
        font = fit_font_size(draw, text, ctx.fonts, avail_w, avail_h, min_size=6,
                              max_size=max(7, ctx.height), path=ctx.options.get("font"))
        text = ellipsize(draw, text, font, avail_w)
        w, h = text_size(draw, text, font)
        draw.text(((ctx.width - w) // 2, (ctx.height - h) // 2), text, font=font, fill=0)
        self.draw_border(image, ctx.options)
        return image

    @staticmethod
    def _strftime(now, fmt: str) -> str:
        for candidate in (fmt, fmt.replace("%-d", "%#d"), fmt.replace("%-d", "%d").replace("%#d", "%d")):
            try:
                return now.strftime(candidate)
            except ValueError:
                continue
        return now.strftime("%Y-%m-%d")  # pragma: no cover - last resort
