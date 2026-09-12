"""shell widget -- runs a *configured* command and displays its output.

SECURITY NOTE: `options.command` is executed with `subprocess.run(...,
shell=False)` (the command is a config-provided argv list, never passed
through `/bin/sh`), but it is still arbitrary-command execution: whatever
you put in your own `dashboard.yaml` runs with your own user's privileges
every refresh cycle. That's the whole point of the widget (uptime, a
personal script, `brew outdated`, ...), but it means:

  * Only ever put commands YOU wrote/trust into your own config file.
  * Never load a dashboard config from an untrusted source (e.g. a config
    file attached to an email, or downloaded from a link someone sent
    you) without reading `widgets:` first for a `type: shell` entry.
  * This widget has no sandboxing beyond your own user account -- it is
    exactly as safe (or not) as running the same command yourself.

Output is truncated and word-wrapped to fit the tile; stderr is ignored
for display purposes (only exit code / timeout / launch failure surface,
as "[error]" / "[timed out]").
"""

from __future__ import annotations

import shlex
import subprocess
from typing import List, Union

from PIL import Image, ImageDraw

from ..fonts import apply_text_case, text_size
from .base import RenderContext, Widget, register
from .text import wrap_text

_DEFAULT_TIMEOUT = 5.0


@register("shell")
class ShellWidget(Widget):
    """options:
        command: a list of argv strings, OR a single string split with
                 shlex.split() (still executed without a shell -- no
                 pipes/redirects/expansion). Required.
        timeout: seconds, default 5
        title: optional header
        text_case: "upper" | "as-is", default "upper" for rows shorter than
            12px (see byok.dashboard.fonts.resolve_text_case) -- set
            "as-is" explicitly if the command's output is case-sensitive
            (a URL, a path, JSON) and legibility at that size is less of a
            concern than preserving it verbatim
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        command: Union[str, List[str], None] = ctx.options.get("command")
        timeout = float(ctx.options.get("timeout", _DEFAULT_TIMEOUT))
        title = ctx.options.get("title", "")

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        y = self.draw_title(draw, ctx, title) if title else 2

        text = apply_text_case(self._run(command, timeout), ctx)
        font = ctx.font(max(9, ctx.height // 10))
        max_width = max(1, ctx.width - 4)
        for line in wrap_text(draw, text, font, max_width):
            _, h = text_size(draw, line, font)
            if y + h > ctx.height - 1:
                break
            draw.text((2, y), line, font=font, fill=0)
            y += h + 1

        self.draw_border(image, ctx.options)
        return image

    @staticmethod
    def _run(command, timeout: float) -> str:
        if not command:
            return ""
        argv = shlex.split(command) if isinstance(command, str) else [str(c) for c in command]
        if not argv:
            return ""
        try:
            proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, shell=False)
        except subprocess.TimeoutExpired:
            return "[timed out]"
        except Exception:
            return "[error]"
        return proc.stdout.strip() or ("[error]" if proc.returncode != 0 else "")
