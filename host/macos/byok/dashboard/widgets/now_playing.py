"""now_playing widget -- artist / track, via an optional external CLI.

macOS has no supported command-line "what's playing" API. This widget
looks for the third-party `nowplaying-cli` tool (https://github.com/kirtan-shah/nowplaying-cli)
on PATH and uses it *only if already installed* -- this module never
installs anything. If it isn't found (or it fails/times out), the widget
falls back to `NullProvider` and renders nothing distracting.
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from typing import Optional

from PIL import Image, ImageDraw

from .base import RenderContext, Widget, register

_TIMEOUT = 2.0


@dataclass
class NowPlaying:
    title: Optional[str] = None
    artist: Optional[str] = None
    playing: bool = False


class NowPlayingProvider:
    def current(self) -> NowPlaying:
        raise NotImplementedError


class NullProvider(NowPlayingProvider):
    def current(self) -> NowPlaying:
        return NowPlaying()


class NowPlayingCliProvider(NowPlayingProvider):
    """Wraps the optional `nowplaying-cli` binary. No-op if it isn't on PATH."""

    def __init__(self, timeout: float = _TIMEOUT):
        self.timeout = timeout
        self._bin = shutil.which("nowplaying-cli")

    @property
    def available(self) -> bool:
        return self._bin is not None

    def current(self) -> NowPlaying:
        if not self._bin:
            return NowPlaying()
        try:
            proc = subprocess.run(
                [self._bin, "get", "title", "artist", "playing"],
                capture_output=True,
                text=True,
                timeout=self.timeout,
            )
        except Exception:
            return NowPlaying()
        if proc.returncode != 0:
            return NowPlaying()
        lines = proc.stdout.splitlines()
        title = lines[0].strip() if len(lines) > 0 else ""
        artist = lines[1].strip() if len(lines) > 1 else ""
        playing = (lines[2].strip().lower() == "true") if len(lines) > 2 else False
        title = title if title and title.lower() != "null" else None
        artist = artist if artist and artist.lower() != "null" else None
        return NowPlaying(title=title, artist=artist, playing=playing)


def default_provider() -> NowPlayingProvider:
    """Best-effort auto-detect: nowplaying-cli if present, else NullProvider."""
    cli = NowPlayingCliProvider()
    return cli if cli.available else NullProvider()


@register("now_playing")
class NowPlayingWidget(Widget):
    """options:
        title: header text, default ""
        text_case: "upper" | "as-is", default "upper" for rows shorter than
            12px (see byok.dashboard.fonts.resolve_text_case)
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        provider: NowPlayingProvider = ctx.providers.get("now_playing") or NullProvider()

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        title = ctx.options.get("title", "")
        y = self.draw_title(draw, ctx, title) if title else 2

        try:
            np = provider.current()
        except Exception:
            np = NowPlaying()

        if np.title:
            text = f"{'▶ ' if np.playing else '⏸ '}{np.title}" + (f" — {np.artist}" if np.artist else "")
        else:
            text = "Not playing"

        from ..fonts import apply_text_case, ellipsize, fit_font_size

        # max_size was `ctx.height // 2` (8px on a 16px row) -- see
        # mac_stats.py's identical fix and
        # docs/troubleshooting.md §6 for why this
        # forced the "Not playing" line into the garbled 8px range.
        #
        # Same follow-up as mac_stats.py: even at the fixed cap's auto-fit
        # size, short rows are still thin-stroke territory, so uppercase by
        # default there (`options.text_case` overrides) -- see
        # byok.dashboard.fonts.apply_text_case.
        text = apply_text_case(text, ctx)
        avail_w = max(1, ctx.width - 4)
        font = fit_font_size(draw, text, ctx.fonts, avail_w, max(1, ctx.height - y - 2),
                              min_size=6, max_size=max(7, ctx.height), path=ctx.options.get("font"))
        # A long "Artist — Title" can still be wider than avail_w at
        # fit_font_size's min_size floor -- ellipsize rather than clip.
        text = ellipsize(draw, text, font, avail_w)
        draw.text((2, y), text, font=font, fill=0)
        self.draw_border(image, ctx.options)
        return image
