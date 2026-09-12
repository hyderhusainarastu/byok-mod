"""reminders widget -- incomplete reminders, via a pluggable provider.

Same shape as `calendar.py`: `NullProvider` (default, no I/O) and
`OsascriptReminderProvider` (shells out to `osascript` against
Reminders.app, with a timeout and broad exception handling so a missing
app, denied permission, or slow response degrades to an empty list rather
than raising).
"""

from __future__ import annotations

import subprocess
from typing import Any, Dict, List, Optional

from PIL import Image, ImageDraw

from ..fonts import text_size
from .base import RenderContext, Widget, register

_OSASCRIPT_TIMEOUT = 5.0

_INCOMPLETE_REMINDERS_SCRIPT = r'''
set outLines to {}
tell application "Reminders"
    try
        set lst to (list named "Reminders")
    on error
        set lst to missing value
    end try
    set theLists to lists
    repeat with l in theLists
        try
            set rs to (reminders of l whose completed is false)
            repeat with r in rs
                set outLines to outLines & {name of r}
            end repeat
        end try
    end repeat
end tell
set AppleScript's text item delimiters to linefeed
return outLines as text
'''


class ReminderProvider:
    def open_reminders(self, now) -> List[Dict[str, Any]]:
        raise NotImplementedError


class NullProvider(ReminderProvider):
    def open_reminders(self, now) -> List[Dict[str, Any]]:
        return []


class OsascriptReminderProvider(ReminderProvider):
    def __init__(self, timeout: float = _OSASCRIPT_TIMEOUT):
        self.timeout = timeout

    def open_reminders(self, now) -> List[Dict[str, Any]]:
        try:
            proc = subprocess.run(
                ["osascript", "-e", _INCOMPLETE_REMINDERS_SCRIPT],
                capture_output=True,
                text=True,
                timeout=self.timeout,
            )
        except Exception:
            return []
        if proc.returncode != 0 or not proc.stdout.strip():
            return []
        return [{"title": line} for line in proc.stdout.splitlines() if line.strip()]


@register("reminders")
class RemindersWidget(Widget):
    """options:
        title: header text, default "To do" ("" for a title-less compact tile)
        max_items: cap the list, default 6
        mode: "list" (default, one line per reminder) | "count" (a single
            "N pending" / "Nothing pending" line -- for a compact row too
            short to list items, e.g. a 16-24px grid row)
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        provider: ReminderProvider = ctx.providers.get("reminders") or NullProvider()
        title = ctx.options.get("title", "To do")
        max_items = int(ctx.options.get("max_items", 6))
        mode = ctx.options.get("mode", "list")

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        y = self.draw_title(draw, ctx, title)

        try:
            items = provider.open_reminders(ctx.now)
        except Exception:
            items = []

        from ..fonts import apply_text_case, ellipsize, fit_font_size

        avail_w = max(1, ctx.width - 4)

        if mode == "count":
            text = f"{len(items)} pending" if items else "Nothing pending"
            text = apply_text_case(text, ctx)
            font = fit_font_size(draw, text, ctx.fonts, avail_w, max(1, ctx.height - y - 2), min_size=6,
                                  max_size=max(7, ctx.height), path=ctx.options.get("font"))
            text = ellipsize(draw, text, font, avail_w)
            draw.text((2, y), text, font=font, fill=0)
            self.draw_border(image, ctx.options)
            return image

        body_font = ctx.font(max(9, ctx.height // 10))
        if not items:
            text = ellipsize(draw, apply_text_case("Nothing pending", ctx), body_font, avail_w)
            if y + text_size(draw, text, body_font)[1] <= ctx.height - 1:
                draw.text((2, y), text, font=body_font, fill=0)
        else:
            for item in items[:max_items]:
                label = f"• {item.get('title', '')}"
                label = ellipsize(draw, apply_text_case(label, ctx), body_font, avail_w)
                if y + text_size(draw, label, body_font)[1] > ctx.height - 1:
                    break
                draw.text((2, y), label, font=body_font, fill=0)
                y += text_size(draw, label, body_font)[1] + 2

        self.draw_border(image, ctx.options)
        return image
