"""mac_stats widget -- a compact bar of CPU / memory / disk / battery.

All data collection happens in `SystemMacStatsProvider`, entirely via
`subprocess` calls to macOS command-line tools (never a private API):

  * CPU:     `top -l 1 -n 0` (falls back to `sysctl -n hw.ncpu` for just a
             core count if `top`'s summary line can't be parsed)
  * Memory:  `vm_stat` (page counts) + `sysctl hw.memsize` for the total
  * Disk:    `shutil.disk_usage(path)` -- stdlib, no subprocess needed
  * Battery: `pmset -g batt`

Every subprocess call has a timeout and is wrapped in try/except; any
failure leaves that one field as `None`, which the widget renders as
"--" rather than crashing or blocking the refresh loop.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass
from typing import Optional

from PIL import Image, ImageDraw

from .base import RenderContext, Widget, register

_TIMEOUT = 3.0


@dataclass
class MacStats:
    cpu_percent: Optional[float] = None
    mem_used_percent: Optional[float] = None
    mem_total_gb: Optional[float] = None
    disk_free_gb: Optional[float] = None
    disk_total_gb: Optional[float] = None
    battery_percent: Optional[int] = None
    battery_charging: Optional[bool] = None


class MacStatsProvider:
    def stats(self) -> MacStats:
        raise NotImplementedError


class NullProvider(MacStatsProvider):
    def stats(self) -> MacStats:
        return MacStats()


class SystemMacStatsProvider(MacStatsProvider):
    def __init__(self, disk_path: str = "/", timeout: float = _TIMEOUT):
        self.disk_path = disk_path
        self.timeout = timeout

    def stats(self) -> MacStats:
        return MacStats(
            cpu_percent=self._cpu_percent(),
            mem_used_percent=self._mem_used_percent(),
            mem_total_gb=self._mem_total_gb(),
            disk_free_gb=self._disk_free_gb(),
            disk_total_gb=self._disk_total_gb(),
            battery_percent=self._battery()[0],
            battery_charging=self._battery()[1],
        )

    def _run(self, argv):
        try:
            return subprocess.run(argv, capture_output=True, text=True, timeout=self.timeout)
        except Exception:
            return None

    def _cpu_percent(self) -> Optional[float]:
        proc = self._run(["top", "-l", "1", "-n", "0"])
        if proc is None or proc.returncode != 0:
            return None
        # Line looks like: "CPU usage: 12.34% user, 5.67% sys, 81.99% idle"
        m = re.search(r"CPU usage:\s*([\d.]+)%\s*user,\s*([\d.]+)%\s*sys,\s*([\d.]+)%\s*idle", proc.stdout)
        if not m:
            return None
        user, sys_, idle = (float(g) for g in m.groups())
        return round(max(0.0, min(100.0, user + sys_)), 1)

    def _mem_total_gb(self) -> Optional[float]:
        proc = self._run(["sysctl", "-n", "hw.memsize"])
        if proc is None or proc.returncode != 0:
            return None
        try:
            return round(int(proc.stdout.strip()) / (1024 ** 3), 1)
        except ValueError:
            return None

    def _mem_used_percent(self) -> Optional[float]:
        proc = self._run(["vm_stat"])
        if proc is None or proc.returncode != 0:
            return None
        page_size = 4096
        m = re.search(r"page size of (\d+) bytes", proc.stdout)
        if m:
            page_size = int(m.group(1))
        pages = {}
        for key in ("Pages free", "Pages active", "Pages inactive", "Pages speculative",
                    "Pages wired down", "Pages occupied by compressor"):
            m = re.search(re.escape(key) + r":\s*(\d+)\.", proc.stdout)
            if m:
                pages[key] = int(m.group(1))
        if not pages:
            return None
        used_pages = (
            pages.get("Pages active", 0)
            + pages.get("Pages wired down", 0)
            + pages.get("Pages occupied by compressor", 0)
        )
        free_pages = pages.get("Pages free", 0) + pages.get("Pages inactive", 0) + pages.get("Pages speculative", 0)
        total_pages = used_pages + free_pages
        if total_pages <= 0:
            return None
        return round(100.0 * used_pages / total_pages, 1)

    def _disk_free_gb(self) -> Optional[float]:
        try:
            return round(shutil.disk_usage(self.disk_path).free / (1024 ** 3), 1)
        except Exception:
            return None

    def _disk_total_gb(self) -> Optional[float]:
        try:
            return round(shutil.disk_usage(self.disk_path).total / (1024 ** 3), 1)
        except Exception:
            return None

    def _battery(self):
        proc = self._run(["pmset", "-g", "batt"])
        if proc is None or proc.returncode != 0:
            return (None, None)
        m = re.search(r"(\d+)%", proc.stdout)
        percent = int(m.group(1)) if m else None
        charging = None
        if "AC Power" in proc.stdout:
            charging = True
        elif "Battery Power" in proc.stdout:
            charging = False
        return (percent, charging)


@register("mac_stats")
class MacStatsWidget(Widget):
    """options:
        title: header text, default "" (no title, stats bar is compact)
        fields: subset/order of ["cpu", "mem", "disk", "battery"], default all
        text_case: "upper" | "as-is", default "upper" for rows shorter than
            12px (see byok.dashboard.fonts.resolve_text_case)
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        provider: MacStatsProvider = ctx.providers.get("mac_stats") or NullProvider()
        fields = ctx.options.get("fields") or ["cpu", "mem", "disk", "battery"]
        title = ctx.options.get("title", "")

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        y = self.draw_title(draw, ctx, title) if title else 2

        try:
            s = provider.stats()
        except Exception:
            s = MacStats()

        parts = []
        if "cpu" in fields:
            parts.append(f"CPU {self._fmt(s.cpu_percent, '%')}")
        if "mem" in fields:
            parts.append(f"MEM {self._fmt(s.mem_used_percent, '%')}")
        if "disk" in fields:
            parts.append(f"DISK {self._fmt(s.disk_free_gb, 'GB free')}")
        if "battery" in fields:
            suffix = ""
            if s.battery_charging:
                suffix = " chg"
            parts.append(f"BATT {self._fmt(s.battery_percent, '%')}{suffix}")

        text = "   ".join(parts)
        from ..fonts import apply_text_case, ellipsize, fit_font_size

        # max_size was `ctx.height // 2` (8px on a 16px row), an
        # undocumented halving with no corresponding constraint elsewhere in
        # this file, unlike date.py/clock.py which use the full row height.
        # That extra cap forced this widget's auto-fit size to exactly 8px,
        # the one size range where anti-aliased-glyph -> hard-1bit
        # thresholding (no dithering for text; see render.py's
        # DITHERED_WIDGET_TYPES) drops thin strokes and garbles text -- see
        # docs/troubleshooting.md §6.
        #
        # Even with that cap fixed, this row's auto-fit size still lands
        # close to the same danger zone on short rows (e.g. 240x80's 16px
        # stats row), and thin lowercase strokes -- the 'c' in "chg" -- can
        # still lose the thresholding fight. `apply_text_case` defaults to
        # uppercasing rows shorter than 12px (`options.text_case` overrides).
        text = apply_text_case(text, ctx)
        avail_w = max(1, ctx.width - 4)
        font = fit_font_size(draw, text, ctx.fonts, avail_w, max(1, ctx.height - y - 2),
                              min_size=6, max_size=max(7, ctx.height), path=ctx.options.get("font"))
        # Selecting all four fields on a narrow/short tile can still be
        # wider than avail_w even at fit_font_size's min_size floor --
        # ellipsize rather than let the last field(s) clip mid-character.
        text = ellipsize(draw, text, font, avail_w)
        draw.text((2, y), text, font=font, fill=0)
        self.draw_border(image, ctx.options)
        return image

    @staticmethod
    def _fmt(value, suffix: str) -> str:
        if value is None:
            return "--"
        if suffix == "%":
            return f"{value:.0f}%" if isinstance(value, float) else f"{value}%"
        return f"{value} {suffix}"
