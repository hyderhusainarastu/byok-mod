"""network widget -- interface name + local IP (no ping, no outbound traffic).

`SystemNetworkProvider` reads the address with `ipconfig getifaddr <iface>`
(fast, local-only; `ifconfig <iface>` is used only to check the interface
exists / report its status when there's no IPv4 address). Nothing here ever
opens a socket or sends a packet.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from typing import Optional

from PIL import Image, ImageDraw

from .base import RenderContext, Widget, register

_TIMEOUT = 2.0


@dataclass
class NetworkInfo:
    interface: Optional[str] = None
    ip_address: Optional[str] = None
    up: Optional[bool] = None


class NetworkProvider:
    def info(self, interface: str) -> NetworkInfo:
        raise NotImplementedError


class NullProvider(NetworkProvider):
    def info(self, interface: str) -> NetworkInfo:
        return NetworkInfo(interface=interface)


class SystemNetworkProvider(NetworkProvider):
    def __init__(self, timeout: float = _TIMEOUT):
        self.timeout = timeout

    def info(self, interface: str) -> NetworkInfo:
        ip_address = None
        try:
            proc = subprocess.run(
                ["ipconfig", "getifaddr", interface], capture_output=True, text=True, timeout=self.timeout
            )
            if proc.returncode == 0:
                ip_address = proc.stdout.strip() or None
        except Exception:
            pass

        up = None
        try:
            proc = subprocess.run(["ifconfig", interface], capture_output=True, text=True, timeout=self.timeout)
            if proc.returncode == 0:
                up = "status: active" in proc.stdout
        except Exception:
            pass

        return NetworkInfo(interface=interface, ip_address=ip_address, up=up)


@register("network")
class NetworkWidget(Widget):
    """options:
        interface: default "en0"
        title: header text, default "" (uses interface name in-line instead)
        text_case: "upper" | "as-is", default "upper" for rows shorter than
            12px (see byok.dashboard.fonts.resolve_text_case)
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        interface = ctx.options.get("interface", "en0")
        provider: NetworkProvider = ctx.providers.get("network") or NullProvider()

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        title = ctx.options.get("title", "")
        y = self.draw_title(draw, ctx, title) if title else 2

        try:
            info = provider.info(interface)
        except Exception:
            info = NetworkInfo(interface=interface)

        if info.ip_address:
            text = f"{info.interface}: {info.ip_address}"
        elif info.up is False:
            text = f"{info.interface}: down"
        else:
            text = f"{info.interface}: --"

        from ..fonts import apply_text_case, ellipsize, fit_font_size

        # max_size was `ctx.height // 2`, the same undocumented halving
        # mac_stats.py/now_playing.py had before
        # docs/troubleshooting.md §6's fix -- date.py/
        # clock.py use the full row height. Matching that fix here, plus the
        # same text_case default (`options.text_case` overrides) for
        # whatever short rows the halved cap was masking.
        text = apply_text_case(text, ctx)
        avail_w = max(1, ctx.width - 4)
        font = fit_font_size(draw, text, ctx.fonts, avail_w, max(1, ctx.height - y - 2),
                              min_size=6, max_size=max(7, ctx.height), path=ctx.options.get("font"))
        text = ellipsize(draw, text, font, avail_w)
        draw.text((2, y), text, font=font, fill=0)
        self.draw_border(image, ctx.options)
        return image
