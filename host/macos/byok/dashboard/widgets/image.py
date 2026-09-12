"""image widget -- a scaled-to-fit image file, letterboxed onto the tile."""

from __future__ import annotations

import os

from PIL import Image, ImageDraw

from .base import RenderContext, Widget, register


@register("image")
class ImageWidget(Widget):
    """options:
        path: path to an image file (required)
        fit: "contain" (default, preserve aspect, letterbox) | "cover"
             (fill and crop) | "stretch"
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        path = ctx.options.get("path")
        fit = ctx.options.get("fit", "contain")
        width, height = max(1, ctx.width), max(1, ctx.height)

        canvas = Image.new("L", (width, height), color=255)
        if not path or not os.path.isfile(path):
            self._draw_placeholder(canvas, "no image")
            self.draw_border(canvas, ctx.options)
            return canvas

        try:
            with Image.open(path) as src:
                gray = src.convert("L")
                if fit == "stretch":
                    scaled = gray.resize((width, height))
                    canvas.paste(scaled, (0, 0))
                elif fit == "cover":
                    scale = max(width / gray.width, height / gray.height)
                    new_size = (max(1, round(gray.width * scale)), max(1, round(gray.height * scale)))
                    scaled = gray.resize(new_size)
                    ox = (new_size[0] - width) // 2
                    oy = (new_size[1] - height) // 2
                    canvas.paste(scaled.crop((ox, oy, ox + width, oy + height)), (0, 0))
                else:  # contain
                    scale = min(width / gray.width, height / gray.height)
                    new_size = (max(1, round(gray.width * scale)), max(1, round(gray.height * scale)))
                    scaled = gray.resize(new_size)
                    canvas.paste(scaled, ((width - new_size[0]) // 2, (height - new_size[1]) // 2))
        except Exception:
            self._draw_placeholder(canvas, "image error")

        self.draw_border(canvas, ctx.options)
        return canvas

    @staticmethod
    def _draw_placeholder(image, text: str) -> None:
        draw = ImageDraw.Draw(image)
        w, h = image.size
        draw.rectangle([2, 2, w - 3, h - 3], outline=180)
        draw.text((6, max(2, h // 2 - 6)), text, fill=128)
