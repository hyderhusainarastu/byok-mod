"""git widget -- branch / status / last commit summary for a configured repo.

Runs `git` directly via subprocess (no shell, no `shell.py`'s "arbitrary
command" surface) against `options["repo"]`, each call individually
timed out and exception-guarded.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from typing import Optional

from PIL import Image, ImageDraw

from ..fonts import fit_font_size, text_size
from .base import RenderContext, Widget, register

_TIMEOUT = 3.0


@dataclass
class GitInfo:
    branch: Optional[str] = None
    dirty: Optional[bool] = None
    last_commit: Optional[str] = None
    error: Optional[str] = None


class GitProvider:
    def info(self, repo_path: str) -> GitInfo:
        raise NotImplementedError


class NullProvider(GitProvider):
    def info(self, repo_path: str) -> GitInfo:
        return GitInfo()


class SystemGitProvider(GitProvider):
    def __init__(self, timeout: float = _TIMEOUT):
        self.timeout = timeout

    def _run(self, repo_path: str, args):
        try:
            return subprocess.run(
                ["git", "-C", repo_path] + args, capture_output=True, text=True, timeout=self.timeout
            )
        except Exception:
            return None

    def info(self, repo_path: str) -> GitInfo:
        if not repo_path:
            return GitInfo(error="no repo configured")

        branch_proc = self._run(repo_path, ["rev-parse", "--abbrev-ref", "HEAD"])
        if branch_proc is None or branch_proc.returncode != 0:
            return GitInfo(error="not a git repo")
        branch = branch_proc.stdout.strip()

        status_proc = self._run(repo_path, ["status", "--porcelain"])
        dirty = bool(status_proc.stdout.strip()) if status_proc and status_proc.returncode == 0 else None

        log_proc = self._run(repo_path, ["log", "-1", "--pretty=%h %s"])
        last_commit = log_proc.stdout.strip() if log_proc and log_proc.returncode == 0 else None

        return GitInfo(branch=branch, dirty=dirty, last_commit=last_commit)


@register("git")
class GitWidget(Widget):
    """options:
        repo: absolute path to a git repo (required for real data)
        title: header text, default "Repo" ("" for a title-less compact tile)
        text_case: "upper" | "as-is", default "upper" for rows shorter than
            12px (see byok.dashboard.fonts.resolve_text_case)
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        repo_path = ctx.options.get("repo", "")
        provider: GitProvider = ctx.providers.get("git") or NullProvider()
        title = ctx.options.get("title", "Repo")

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        y = self.draw_title(draw, ctx, title)

        try:
            info = provider.info(repo_path)
        except Exception:
            info = GitInfo(error="provider error")

        from ..fonts import apply_text_case, ellipsize

        avail_w = max(1, ctx.width - 4)
        avail_h_total = max(1, ctx.height - y)
        if info.error or not info.branch:
            text = apply_text_case(info.error or "--", ctx)
            font = fit_font_size(draw, text, ctx.fonts, avail_w, avail_h_total, min_size=6,
                                  max_size=max(7, ctx.height), path=ctx.options.get("font"))
            text = ellipsize(draw, text, font, avail_w)
            draw.text((2, y), text, font=font, fill=0)
        else:
            dirty_mark = "*" if info.dirty else ""
            branch_text = apply_text_case(f"{info.branch}{dirty_mark}", ctx)

            # A branch line + a commit-summary line only both read cleanly
            # with real room for two lines -- halving a short row (e.g. a
            # 16px grid row) in two forces both fonts down into the same
            # "too small to read" territory this widget's own text_case
            # default exists to mitigate elsewhere, and unlike a
            # single-value stat, a commit summary trimmed hard enough to
            # fit an ~7px line is not worth showing at all (see
            # docs/host-tools.md's git widget section, and the work.yaml
            # example this threshold was picked against). Below that,
            # collapse to a single branch line sized to the *full*
            # available height instead of an artificially halved one, and
            # skip the commit line entirely rather than show it garbled.
            two_line_min_height = 22
            if info.last_commit and avail_h_total >= two_line_min_height:
                row_h = max(1, avail_h_total // 2)
                font = fit_font_size(draw, branch_text, ctx.fonts, avail_w, row_h, min_size=6,
                                      max_size=max(7, row_h), path=ctx.options.get("font"))
                branch_text = ellipsize(draw, branch_text, font, avail_w)
                draw.text((2, y), branch_text, font=font, fill=0)
                y += text_size(draw, branch_text, font)[1] + 1
                commit_font = ctx.font(max(6, font.size if hasattr(font, "size") else 8))
                commit = ellipsize(draw, apply_text_case(info.last_commit, ctx), commit_font, avail_w)
                draw.text((2, y), commit, font=commit_font, fill=0)
            else:
                font = fit_font_size(draw, branch_text, ctx.fonts, avail_w, avail_h_total, min_size=6,
                                      max_size=max(7, avail_h_total), path=ctx.options.get("font"))
                # fit_font_size never grows past what fits, but at its own
                # min_size floor an especially long branch name can still
                # be wider than avail_w -- ellipsize is a no-op otherwise.
                branch_text = ellipsize(draw, branch_text, font, avail_w)
                draw.text((2, y), branch_text, font=font, fill=0)

        self.draw_border(image, ctx.options)
        return image
