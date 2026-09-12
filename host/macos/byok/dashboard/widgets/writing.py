"""writing widget -- manuscript stats read from the device itself.

Unlike every other widget in this package, the data here doesn't come
from a Mac-side subprocess/AppleScript/file read: it comes from the
*device*, over the BYOK Link, via `GET_DOCSTATS` -> `DOCSTATS`
(`byok.device.Device.get_docstats()`, `byok.proto.Type.GET_DOCSTATS`,
docs/protocol.md §6.4b, v1.2/firmware 0.1.14). The wire layout
implemented against here (20 B: `u32 files, words, bytes, newest_epoch,
words_today`, all LE) came from the firmware side's own definition for
this message, already fixed, and needed no correction; the message's *type code* did move during reconciliation
(0x29 -> 0x2A) -- see `proto.py`'s `Type` enum comment and
`docs/host-tools.md`'s "Protocol reconciliation" section for the full
list of what was guessed first and corrected against the real spec.

Same `Provider`/`NullProvider` contract as every other widget here
(`docs/host-tools.md` §12): `WritingProvider.stats()` never raises,
`NullProvider` does no I/O, and a real provider
(`DeviceDocStatsProvider`) catches its own failure (device not connected,
firmware doesn't implement `GET_DOCSTATS` yet -> `NACK/E_UNKNOWN_TYPE`,
a link timeout mid-cycle) and degrades to "--" rather than raising.

`DeviceDocStatsProvider` is throttled (`min_interval_s`, default 30s):
`GET_DOCSTATS` is a synchronous request/reply round-trip over the same
link the dashboard loop uses to draw frames (docs/protocol.md §7.6's
250ms ACK timeout, x2 retries worst case), so polling it every render
cycle (as fast as every few seconds on some configs) would add avoidable
latency to *every* frame, not just the ones where the manuscript stats
could plausibly have changed. A stale-by-up-to-30s word count is a
non-issue for a "how's the manuscript going" glance; a dashboard that
stutters because it's blocking on a docstats round-trip every frame is a
real regression.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass
from typing import Optional

from PIL import Image, ImageDraw

from .base import RenderContext, Widget, register

logger = logging.getLogger("byok.dashboard.writing")

DEFAULT_MIN_INTERVAL_S = 30.0
DEFAULT_DAILY_GOAL_WORDS = 500


@dataclass
class WritingStats:
    files: Optional[int] = None
    words: Optional[int] = None
    bytes: Optional[int] = None
    newest_epoch: Optional[float] = None
    words_today: Optional[int] = None
    error: Optional[str] = None


class WritingProvider:
    def stats(self) -> WritingStats:
        raise NotImplementedError


class NullProvider(WritingProvider):
    """No device, no I/O -- what offline previews/tests use, and what a
    dashboard running against firmware that predates GET_DOCSTATS
    degrades to (see DeviceDocStatsProvider's NACK/E_UNKNOWN_TYPE catch,
    which returns this same all-empty shape rather than an error field
    the widget would render as visible "error" text -- "not implemented
    yet" and "no manuscript data" look identical to the reader here,
    which is the right call for something this non-critical)."""

    def stats(self) -> WritingStats:
        return WritingStats()


class DeviceDocStatsProvider(WritingProvider):
    """Wraps a connected `byok.device.Device` (or any object exposing the
    same `.get_docstats() -> device.DocStats` method -- duck-typed
    deliberately, so tests can pass a bare stand-in instead of a real
    `Device` wrapping a fake transport). See module docstring for the
    throttling rationale.
    """

    def __init__(self, device, min_interval_s: float = DEFAULT_MIN_INTERVAL_S, clock=time.monotonic):
        self.device = device
        self.min_interval_s = min_interval_s
        self.clock = clock
        self._cached: Optional[WritingStats] = None
        self._last_fetch: Optional[float] = None

    def stats(self) -> WritingStats:
        now = self.clock()
        if self._cached is not None and self._last_fetch is not None and (now - self._last_fetch) < self.min_interval_s:
            return self._cached

        try:
            raw = self.device.get_docstats()
            result = WritingStats(
                files=raw.files, words=raw.words, bytes=raw.bytes,
                newest_epoch=float(raw.newest_epoch) if raw.newest_epoch else None,
                words_today=raw.words_today,
            )
        except Exception as exc:  # noqa: BLE001 - any transport/NACK failure degrades, never raises
            logger.debug("writing: GET_DOCSTATS failed", exc_info=True)
            result = WritingStats(error=str(exc))

        self._cached = result
        self._last_fetch = now
        return result


def _relative_time(now_epoch: float, then_epoch: Optional[float]) -> str:
    if not then_epoch:
        return "--"
    delta = max(0, int(now_epoch - then_epoch))
    if delta < 60:
        return "now" if delta < 5 else f"{delta}s ago"
    if delta < 3600:
        return f"{delta // 60}m ago"
    if delta < 86400:
        return f"{delta // 3600}h ago"
    return f"{delta // 86400}d ago"


def _fmt_words(n: Optional[int]) -> str:
    if n is None:
        return "--"
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f}M"
    if n >= 10_000:
        return f"{n / 1_000:.1f}K"
    return str(n)


@register("writing")
class WritingWidget(Widget):
    """options:
        title: header text, default "Writing" ("" for a title-less compact
            tile)
        daily_goal_words: the daily-goal bar's denominator, default 500
        text_case: "upper" | "as-is", default "upper" for rows shorter than
            12px (see byok.dashboard.fonts.resolve_text_case)

    `NullProvider` (no device, or firmware without GET_DOCSTATS) renders
    "-- WORDS", matching every other widget's "no data" convention rather
    than a visible error tile.
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        provider: WritingProvider = ctx.providers.get("writing") or NullProvider()
        title = ctx.options.get("title", "Writing")
        goal = int(ctx.options.get("daily_goal_words", DEFAULT_DAILY_GOAL_WORDS)) or DEFAULT_DAILY_GOAL_WORDS

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        y = self.draw_title(draw, ctx, title) if title else 2

        try:
            s = provider.stats()
        except Exception:
            s = WritingStats()

        now_epoch = _epoch_from_ctx_now(ctx.now)

        from ..fonts import apply_text_case, ellipsize, fit_font_size

        avail_w = max(1, ctx.width - 4)
        avail_h = max(1, ctx.height - y - 2)

        if s.words is None:
            text = apply_text_case("-- words", ctx)
            font = fit_font_size(draw, text, ctx.fonts, avail_w, avail_h, min_size=6,
                                  max_size=max(7, avail_h), path=ctx.options.get("font"))
            text = ellipsize(draw, text, font, avail_w)
            draw.text((2, y), text, font=font, fill=0)
            self.draw_border(image, ctx.options)
            return image

        last_edit = _relative_time(now_epoch, s.newest_epoch)
        line1 = apply_text_case(
            f"{_fmt_words(s.words)} WORDS   {s.files if s.files is not None else '--'} FILES   {last_edit}", ctx
        )
        line1_compact = apply_text_case(
            f"{_fmt_words(s.words)} WORDS  TODAY {_fmt_words(s.words_today)}  {last_edit}", ctx
        )

        bar_h = 8
        two_line_min_height = 22
        if avail_h >= two_line_min_height:
            row_h = max(1, avail_h - bar_h - 2)
            font = fit_font_size(draw, line1, ctx.fonts, avail_w, row_h, min_size=6,
                                  max_size=max(7, row_h), path=ctx.options.get("font"))
            line1 = ellipsize(draw, line1, font, avail_w)
            draw.text((2, y), line1, font=font, fill=0)

            bar_y = ctx.height - bar_h - 2
            words_today = s.words_today or 0
            frac = min(1.0, words_today / goal) if goal > 0 else 0.0
            bar_w = min(avail_w - 90, avail_w)
            if bar_w > 10:
                self.draw_bar(draw, 2, bar_y, bar_w, bar_h, frac)
                label_font = ctx.font(max(6, bar_h))
                label = apply_text_case(f"TODAY {_fmt_words(words_today)}/{_fmt_words(goal)}", ctx)
                label = ellipsize(draw, label, label_font, avail_w - bar_w - 6)
                draw.text((2 + bar_w + 4, bar_y - 1), label, font=label_font, fill=0)
        else:
            font = fit_font_size(draw, line1_compact, ctx.fonts, avail_w, avail_h, min_size=6,
                                  max_size=max(7, avail_h), path=ctx.options.get("font"))
            line1_compact = ellipsize(draw, line1_compact, font, avail_w)
            draw.text((2, y), line1_compact, font=font, fill=0)

        self.draw_border(image, ctx.options)
        return image


def _epoch_from_ctx_now(now) -> float:
    """Converts `ctx.now` (a naive local `datetime`) to an epoch float, since
    every provider field here is a `time.time()`-comparable epoch."""
    try:
        return time.mktime(now.timetuple())
    except (AttributeError, ValueError, OverflowError):
        return time.time()
