"""CLI: render a dashboard config to a PNG, for eyeballing it before it
ever touches a device.

    python3 -m byok.dashboard.preview --config default_dashboard.yaml \
        --width 320 --height 240 --out preview.png

`--width`/`--height` override `display.width`/`display.height` in the
config (which are themselves optional -- the config is geometry-agnostic).
`--bpp` overrides `display.bpp`. `--no-quantize` skips the 1bpp/2bpp
quantization step and writes the raw anti-aliased grayscale composite
instead, useful for judging text layout independent of the panel's bit
depth. `--repo` overrides the `git` widget's `options.repo` (handy since
the shipped default config points it at ".", i.e. wherever you run the
command from) and `--now` lets you render as of a fixed timestamp
(`YYYY-MM-DDTHH:MM:SS`) for reproducible screenshots.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import sys
from typing import Optional

from . import config as config_mod
from . import render as render_mod
from .fonts import FontSet
from .layout import render_dashboard
from .widgets import calendar as calendar_widget
from .widgets import git as git_widget
from .widgets import mac_stats as mac_stats_widget
from .widgets import network as network_widget
from .widgets import now_playing as now_playing_widget
from .widgets import reminders as reminders_widget


def _live_providers() -> dict:
    """Real (not Null) providers, best-effort -- what the CLI uses by
    default so `preview.py` shows what the dashboard will actually say on
    this Mac, not a wall of placeholders. Every provider here already
    swallows its own I/O failures.

    `writing` (widgets/writing.py) is deliberately not included here --
    its only real provider (`DeviceDocStatsProvider`) needs a connected
    `byok.device.Device`, which this module (also used by the device-free
    `preview.py` CLI) never has; `cli.py cmd_dashboard` wires it in
    separately, after `_connect()`, alongside this dict's entries.
    """
    return {
        "calendar": calendar_widget.OsascriptCalendarProvider(),
        "reminders": reminders_widget.OsascriptReminderProvider(),
        "mac_stats": mac_stats_widget.SystemMacStatsProvider(),
        "network": network_widget.SystemNetworkProvider(),
        "now_playing": now_playing_widget.default_provider(),
        "git": git_widget.SystemGitProvider(),
    }


def _apply_repo_override(cfg: config_mod.DashboardConfig, repo: Optional[str]) -> None:
    if not repo:
        return
    for w in cfg.widgets:
        if w.type == "git":
            w.options["repo"] = repo


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, help="path to a dashboard YAML config")
    parser.add_argument("--width", type=int, default=None, help="override display.width")
    parser.add_argument("--height", type=int, default=None, help="override display.height")
    parser.add_argument("--bpp", type=int, choices=(1, 2), default=None, help="override display.bpp")
    parser.add_argument("--out", required=True, help="output PNG path")
    parser.add_argument("--no-quantize", action="store_true", help="skip bpp quantization, write raw grayscale")
    parser.add_argument("--repo", default=None, help="override the git widget's repo path")
    parser.add_argument("--now", default=None, help="ISO timestamp to render as 'now' (for reproducible output)")
    parser.add_argument("--offline", action="store_true", help="use Null providers only, no subprocess/AppleScript calls")
    return parser


def main(argv=None) -> int:
    args = build_arg_parser().parse_args(argv)
    cfg = config_mod.load(args.config)
    _apply_repo_override(cfg, args.repo)

    width = args.width or cfg.display.width or 320
    height = args.height or cfg.display.height or 240
    bpp = args.bpp or cfg.display.bpp

    now = _dt.datetime.fromisoformat(args.now) if args.now else _dt.datetime.now()
    providers = {} if args.offline else _live_providers()
    fonts = FontSet(ttf_path=cfg.fonts.get("path"))

    if args.no_quantize:
        image = render_dashboard(cfg, width, height, now, providers=providers, fonts=fonts)
    else:
        image = render_mod.render_and_quantize(cfg, width, height, bpp, now, providers=providers, fonts=fonts)

    image.save(args.out)
    print(f"wrote {args.out} ({image.width}x{image.height}, {'raw' if args.no_quantize else f'{bpp}bpp'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
