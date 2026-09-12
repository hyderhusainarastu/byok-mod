"""calendar widget -- today's events, via a pluggable provider.

Two providers ship here:

  * `NullProvider` -- always returns no events. Used by default in tests
    and anywhere Calendar.app access isn't wanted/available.
  * `OsascriptCalendarProvider` -- shells out to `osascript` to ask
    Calendar.app for today's events. This talks to a real GUI app and can
    be slow (or pop a permissions dialog the first time), so it always
    runs with a timeout and any failure (osascript missing, Calendar.app
    not running, permission denied, timeout, bad output) is caught and
    treated as "no events" rather than raising.

`ctx.providers["calendar"]` supplies the provider; `CalendarWidget` falls
back to `NullProvider()` if none is configured, so it's always safe to
render with a bare `RenderContext`.
"""

from __future__ import annotations

import subprocess
from typing import Any, Dict, List

from PIL import Image, ImageDraw

from ..fonts import text_size
from .base import RenderContext, Widget, register

_OSASCRIPT_TIMEOUT = 5.0

# Lists today's events across every calendar, "title|HH:MM|allday" per line.
# Kept intentionally simple (no attendee/location lookups) to stay fast.
_TODAY_EVENTS_SCRIPT = r'''
set outLines to {}
tell application "Calendar"
    set today to current date
    set today's time to 0
    set tomorrow to today + 1 * days
    repeat with cal in calendars
        try
            set evts to (every event of cal whose start date is greater than or equal to today and start date is less than tomorrow)
            repeat with e in evts
                set t to summary of e
                set isAllDay to allday event of e
                if isAllDay then
                    set outLines to outLines & {t & "|" & "" & "|" & "1"}
                else
                    set sd to start date of e
                    set hh to text -2 thru -1 of ("0" & (hours of sd))
                    set mm to text -2 thru -1 of ("0" & (minutes of sd))
                    set outLines to outLines & {t & "|" & hh & ":" & mm & "|" & "0"}
                end if
            end repeat
        end try
    end repeat
end tell
set AppleScript's text item delimiters to linefeed
return outLines as text
'''


class CalendarProvider:
    def today_events(self, now) -> List[Dict[str, Any]]:
        raise NotImplementedError


class NullProvider(CalendarProvider):
    def today_events(self, now) -> List[Dict[str, Any]]:
        return []


class OsascriptCalendarProvider(CalendarProvider):
    def __init__(self, timeout: float = _OSASCRIPT_TIMEOUT):
        self.timeout = timeout

    def today_events(self, now) -> List[Dict[str, Any]]:
        try:
            proc = subprocess.run(
                ["osascript", "-e", _TODAY_EVENTS_SCRIPT],
                capture_output=True,
                text=True,
                timeout=self.timeout,
            )
        except Exception:
            return []
        if proc.returncode != 0 or not proc.stdout.strip():
            return []
        events: List[Dict[str, Any]] = []
        for line in proc.stdout.splitlines():
            parts = line.split("|")
            if len(parts) != 3:
                continue
            title, when, allday = parts
            events.append({"title": title, "time": when, "all_day": allday == "1"})
        return events


@register("calendar")
class CalendarWidget(Widget):
    """options:
        title: header text, default "Today" ("" for a title-less compact tile)
        max_events: cap the list, default 5 -- set to 1 for a single
            "next event" line in a compact row (see examples/work.yaml)
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        provider: CalendarProvider = ctx.providers.get("calendar") or NullProvider()
        title = ctx.options.get("title", "Today")
        max_events = int(ctx.options.get("max_events", 5))

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        y = self.draw_title(draw, ctx, title)

        try:
            events = provider.today_events(ctx.now)
        except Exception:
            events = []

        # Chronological order, all-day events first -- providers (real or
        # fake) make no ordering guarantee of their own (the AppleScript
        # provider walks calendar-by-calendar, not time-by-time), so
        # `max_events: 1` (a compact "next event" row -- examples/work.yaml)
        # would otherwise show whichever event happened to come first out
        # of Calendar.app rather than the actually-next one. Does not filter
        # out events already in the past today -- "earliest today", not
        # strictly "still upcoming".
        events = sorted(events, key=lambda ev: (0, "") if ev.get("all_day") else (1, ev.get("time") or "99:99"))

        from ..fonts import apply_text_case, ellipsize

        avail_w = max(1, ctx.width - 4)
        body_font = ctx.font(max(9, ctx.height // 10))
        if not events:
            text = ellipsize(draw, apply_text_case("No events", ctx), body_font, avail_w)
            if y + text_size(draw, text, body_font)[1] <= ctx.height - 1:
                draw.text((2, y), text, font=body_font, fill=0)
        else:
            for ev in events[:max_events]:
                when = ev.get("time") or "all-day" if ev.get("all_day") else ev.get("time", "")
                label = f"{when}  {ev.get('title', '')}".strip()
                label = ellipsize(draw, apply_text_case(label, ctx), body_font, avail_w)
                if y + text_size(draw, label, body_font)[1] > ctx.height - 1:
                    break
                draw.text((2, y), label, font=body_font, fill=0)
                y += text_size(draw, label, body_font)[1] + 2

        self.draw_border(image, ctx.options)
        return image
