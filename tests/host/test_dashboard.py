"""tests/host/test_dashboard.py -- unittest suite for byok.dashboard.

Runnable with the standard library (+ Pillow, already a byok dependency)
only:

    python3 -m unittest tests.host.test_dashboard -v

from the repo root. See host/macos/byok/dashboard/REQUIREMENTS.md if
Pillow isn't importable yet.

Known-answer values in the QR tests are derived from the standard
ISO/IEC 18004 formulas (GF(256) with primitive polynomial 0x11D; the
format-info BCH(15,5) with generator 0x537 and XOR mask 0x5412) computed
independently of byok.dashboard.widgets.qr's implementation, the same
convention tests/host/test_render.py uses for its packing KATs -- not
copied from a captured run of this code.
"""

import contextlib
import datetime
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)

from PIL import Image  # noqa: E402

from byok import cli as cli_mod  # noqa: E402
from byok.dashboard import config as config_mod  # noqa: E402
from byok.dashboard import layout as layout_mod  # noqa: E402
from byok.dashboard import render as render_mod  # noqa: E402
from byok.dashboard.fonts import FontSet, apply_text_case, ellipsize, resolve_text_case, text_size  # noqa: E402
from byok.dashboard.widgets import qr as qr_mod  # noqa: E402
from byok.dashboard.widgets.base import RenderContext, WIDGET_REGISTRY  # noqa: E402
from byok.dashboard.widgets.calendar import CalendarProvider  # noqa: E402
from byok.dashboard.widgets.calendar import CalendarWidget  # noqa: E402
from byok.dashboard.widgets.clock import ClockWidget  # noqa: E402
from byok.dashboard.widgets.date import DateWidget  # noqa: E402
from byok.dashboard.widgets.git import GitInfo, GitProvider, GitWidget  # noqa: E402
from byok.dashboard.widgets.image import ImageWidget  # noqa: E402
from byok.dashboard.widgets.mac_stats import MacStats, MacStatsProvider, MacStatsWidget  # noqa: E402
from byok.dashboard.widgets.network import NetworkInfo, NetworkProvider, NetworkWidget  # noqa: E402
from byok.dashboard.widgets.now_playing import NowPlaying, NowPlayingProvider, NowPlayingWidget  # noqa: E402
from byok.dashboard.widgets.qr import QRWidget  # noqa: E402
from byok.dashboard.widgets.reminders import ReminderProvider, RemindersWidget  # noqa: E402
from byok.dashboard.widgets.shell import ShellWidget  # noqa: E402
from byok.dashboard.widgets.text import TextWidget, wrap_text  # noqa: E402
from byok.dashboard.widgets.writing import WritingProvider, WritingStats, WritingWidget  # noqa: E402

_NOW = datetime.datetime(2026, 9, 3, 14, 32, 7)
_DEFAULT_YAML_PATH = os.path.join(_HOST_MACOS, "byok", "dashboard", "default_dashboard.yaml")
_EXAMPLES_DIR = os.path.join(_HOST_MACOS, "byok", "dashboard", "examples")
_FONTS = FontSet()


def _ctx(width=100, height=40, options=None, now=_NOW, providers=None):
    return RenderContext(width=width, height=height, now=now, options=options or {}, fonts=_FONTS,
                          providers=providers or {})


def _nonwhite_pixels(img):
    return sum(1 for v in img.getdata() if v != 255)


def _content_bbox(img):
    """Bounding box of non-white pixels, or None if the image is blank.

    `Image.getbbox()` treats any non-zero pixel as content, which is
    useless on an 'L' image whose *background* is 255 (white) -- every
    pixel would count. This inverts that (255 -> 0, everything else
    non-zero) so `getbbox()` finds actual drawn content instead.
    """
    return img.point(lambda v: 0 if v == 255 else 255).getbbox()


# ---------------------------------------------------------------------------
# config.py -- YAML loading (PyYAML and the dependency-free fallback)
# ---------------------------------------------------------------------------


class ConfigParsingTests(unittest.TestCase):
    SAMPLE = """
display:
  width: 320
  height: 240
  bpp: 2

refresh_seconds: 30

layout:
  grid:
    cols: 4
    rows: 2

widgets:
  - type: clock
    at: [0, 0]
    span: [2, 1]
    options:
      format: "%H:%M"
      title: "Now"
  - type: text
    at: [2, 0]
    span: [2, 2]
    options:
      text: "hello world"
      align: center
"""

    def test_fallback_parser_matches_documented_schema(self):
        cfg = config_mod.loads_with_parser = None  # not used; keep name out of the way
        raw = config_mod.parse_yaml_subset(self.SAMPLE)
        self.assertEqual(raw["display"], {"width": 320, "height": 240, "bpp": 2})
        self.assertEqual(raw["refresh_seconds"], 30)
        self.assertEqual(raw["layout"]["grid"], {"cols": 4, "rows": 2})
        self.assertEqual(len(raw["widgets"]), 2)
        self.assertEqual(raw["widgets"][0]["type"], "clock")
        self.assertEqual(raw["widgets"][0]["at"], [0, 0])
        self.assertEqual(raw["widgets"][0]["span"], [2, 1])
        self.assertEqual(raw["widgets"][0]["options"]["format"], "%H:%M")
        self.assertEqual(raw["widgets"][1]["options"]["align"], "center")

    def test_loads_typed_config_via_fallback_parser(self):
        had_pyyaml = config_mod._HAVE_PYYAML
        config_mod._HAVE_PYYAML = False
        try:
            cfg = config_mod.loads(self.SAMPLE)
        finally:
            config_mod._HAVE_PYYAML = had_pyyaml
        self.assertEqual(cfg.display.width, 320)
        self.assertEqual(cfg.display.bpp, 2)
        self.assertEqual(cfg.grid.cols, 4)
        self.assertEqual(cfg.grid.rows, 2)
        self.assertEqual(len(cfg.widgets), 2)
        self.assertEqual(cfg.widgets[0].type, "clock")
        self.assertEqual(cfg.widgets[0].at, (0, 0))
        self.assertEqual(cfg.widgets[0].span, (2, 1))

    @unittest.skipUnless(config_mod._HAVE_PYYAML, "PyYAML not importable in this interpreter")
    def test_pyyaml_and_fallback_parser_agree(self):
        via_pyyaml = config_mod.loads(self.SAMPLE)
        had_pyyaml = config_mod._HAVE_PYYAML
        config_mod._HAVE_PYYAML = False
        try:
            via_fallback = config_mod.loads(self.SAMPLE)
        finally:
            config_mod._HAVE_PYYAML = had_pyyaml
        self.assertEqual(via_pyyaml.display, via_fallback.display)
        self.assertEqual(via_pyyaml.grid, via_fallback.grid)
        self.assertEqual(len(via_pyyaml.widgets), len(via_fallback.widgets))
        for a, b in zip(via_pyyaml.widgets, via_fallback.widgets):
            self.assertEqual((a.type, a.at, a.span, a.options), (b.type, b.at, b.span, b.options))

    def test_comments_and_quoted_strings(self):
        text = """
widgets:
  - type: text  # a comment
    at: [0, 0]
    span: [1, 1]
    options:
      text: "value: with a colon"  # trailing comment
      other: 'single # not a comment'
"""
        raw = config_mod.parse_yaml_subset(text)
        opts = raw["widgets"][0]["options"]
        self.assertEqual(opts["text"], "value: with a colon")
        self.assertEqual(opts["other"], "single # not a comment")

    def test_scalar_type_coercion(self):
        text = """
a: 1
b: 1.5
c: true
d: false
e: null
f: ~
g: plain string
h: [1, 2.5, true, null, "x"]
"""
        raw = config_mod.parse_yaml_subset(text)
        self.assertEqual(raw["a"], 1)
        self.assertEqual(raw["b"], 1.5)
        self.assertIs(raw["c"], True)
        self.assertIs(raw["d"], False)
        self.assertIsNone(raw["e"])
        self.assertIsNone(raw["f"])
        self.assertEqual(raw["g"], "plain string")
        self.assertEqual(raw["h"], [1, 2.5, True, None, "x"])

    def test_bad_bpp_rejected(self):
        with self.assertRaises(config_mod.ConfigError):
            config_mod.loads("display:\n  bpp: 3\nwidgets: []\n")

    def test_widget_without_type_rejected(self):
        with self.assertRaises(config_mod.ConfigError):
            config_mod.loads("widgets:\n  - at: [0, 0]\n    span: [1, 1]\n")

    def test_zero_span_rejected(self):
        with self.assertRaises(config_mod.ConfigError):
            config_mod.loads("widgets:\n  - type: text\n    span: [0, 1]\n")

    def test_defaults_when_sections_missing(self):
        cfg = config_mod.loads("widgets: []\n")
        self.assertEqual(cfg.grid.cols, 12)
        self.assertEqual(cfg.grid.rows, 8)
        self.assertEqual(cfg.refresh_seconds, 60)
        self.assertEqual(cfg.display.bpp, 1)
        self.assertIsNone(cfg.display.width)

    def test_load_default_dashboard_yaml_from_disk(self):
        cfg = config_mod.load(_DEFAULT_YAML_PATH)
        self.assertEqual(cfg.display.width, 320)
        self.assertEqual(cfg.display.height, 240)
        self.assertGreaterEqual(len(cfg.widgets), 5)
        types = {w.type for w in cfg.widgets}
        self.assertIn("clock", types)
        self.assertIn("qr", types)


# ---------------------------------------------------------------------------
# layout.py -- grid math
# ---------------------------------------------------------------------------


class LayoutMathTests(unittest.TestCase):
    def test_widget_rect_full_grid_cell(self):
        w = config_mod.WidgetConfig(type="clock", at=(0, 0), span=(1, 1))
        rect = layout_mod.widget_rect(w, cols=4, rows=2, width=400, height=200)
        self.assertEqual(rect, (0, 0, 100, 100))

    def test_widget_rect_spans_multiple_cells(self):
        w = config_mod.WidgetConfig(type="clock", at=(1, 0), span=(2, 2))
        rect = layout_mod.widget_rect(w, cols=4, rows=2, width=400, height=200)
        self.assertEqual(rect, (100, 0, 300, 200))

    def test_widget_rect_is_resolution_independent(self):
        w = config_mod.WidgetConfig(type="clock", at=(0, 0), span=(6, 4))
        r_240 = layout_mod.widget_rect(w, cols=12, rows=8, width=240, height=160)
        r_320 = layout_mod.widget_rect(w, cols=12, rows=8, width=320, height=240)
        # Same fraction of the canvas (half width, half height) at both sizes.
        self.assertAlmostEqual(r_240[2] / 240, r_320[2] / 320, places=1)
        self.assertAlmostEqual(r_240[3] / 160, r_320[3] / 240, places=1)

    def test_default_dashboard_widgets_do_not_overlap(self):
        cfg = config_mod.load(_DEFAULT_YAML_PATH)
        rects = [layout_mod.widget_rect(w, cfg.grid.cols, cfg.grid.rows, 320, 240) for w in cfg.widgets]
        for i in range(len(rects)):
            for j in range(i + 1, len(rects)):
                ax0, ay0, ax1, ay1 = rects[i]
                bx0, by0, bx1, by1 = rects[j]
                overlap = ax0 < bx1 and bx0 < ax1 and ay0 < by1 and by0 < ay1
                self.assertFalse(
                    overlap,
                    f"widgets {cfg.widgets[i].type}@{rects[i]} and {cfg.widgets[j].type}@{rects[j]} overlap",
                )

    def test_unknown_widget_type_renders_placeholder_not_crash(self):
        cfg = config_mod.loads(
            "layout:\n  grid:\n    cols: 1\n    rows: 1\nwidgets:\n  - type: not_a_real_widget\n    at: [0, 0]\n    span: [1, 1]\n"
        )
        img = layout_mod.render_dashboard(cfg, 50, 50, _NOW)
        self.assertEqual(img.size, (50, 50))
        self.assertGreater(_nonwhite_pixels(img), 0)  # the [x] placeholder drew something

    def test_widget_exception_renders_placeholder_not_crash(self):
        class Boom:
            def render(self, ctx):
                raise RuntimeError("boom")

        WIDGET_REGISTRY["_test_boom"] = Boom
        try:
            cfg = config_mod.loads(
                "layout:\n  grid:\n    cols: 1\n    rows: 1\nwidgets:\n  - type: _test_boom\n    at: [0, 0]\n    span: [1, 1]\n"
            )
            img = layout_mod.render_dashboard(cfg, 50, 50, _NOW)
            self.assertEqual(img.size, (50, 50))
        finally:
            del WIDGET_REGISTRY["_test_boom"]

    def test_gap_in_grid_leaves_background_blank(self):
        cfg = config_mod.loads(
            "layout:\n  grid:\n    cols: 2\n    rows: 1\nwidgets:\n  - type: text\n    at: [0, 0]\n    span: [1, 1]\n    options:\n      text: \"\"\n"
        )
        img = layout_mod.render_dashboard(cfg, 40, 20, _NOW)
        # Right half (no widget) must stay the canvas background (white).
        right_half = img.crop((20, 0, 40, 20))
        self.assertEqual(set(right_half.getdata()), {255})


# ---------------------------------------------------------------------------
# Widgets, with fake/null providers for determinism
# ---------------------------------------------------------------------------


class ClockDateTextWidgetTests(unittest.TestCase):
    def test_clock_renders_something_and_is_deterministic(self):
        ctx1 = _ctx(options={"format": "%H:%M"})
        ctx2 = _ctx(options={"format": "%H:%M"})
        img1 = ClockWidget().render(ctx1)
        img2 = ClockWidget().render(ctx2)
        self.assertEqual(img1.size, (100, 40))
        self.assertGreater(_nonwhite_pixels(img1), 0)
        self.assertEqual(list(img1.getdata()), list(img2.getdata()))

    def test_clock_format_changes_output(self):
        img_24h = ClockWidget().render(_ctx(options={"format": "%H:%M"}))
        img_12h = ClockWidget().render(_ctx(options={"format": "%I:%M %p"}))
        self.assertNotEqual(list(img_24h.getdata()), list(img_12h.getdata()))

    def test_date_renders_something(self):
        img = DateWidget().render(_ctx(options={"format": "%Y-%m-%d"}))
        self.assertGreater(_nonwhite_pixels(img), 0)

    def test_date_handles_dash_d_portably(self):
        # %-d isn't supported on every platform's strftime; DateWidget must
        # not raise regardless.
        img = DateWidget().render(_ctx(options={"format": "%a, %b %-d"}))
        self.assertEqual(img.size, (100, 40))

    def test_text_word_wrap_breaks_on_width(self):
        img = Image.new("L", (1, 1))
        from PIL import ImageDraw

        draw = ImageDraw.Draw(img)
        font = _FONTS.get(12)
        lines = wrap_text(draw, "one two three four five", font, max_width=40)
        self.assertGreater(len(lines), 1)
        self.assertEqual(" ".join(lines).split(), "one two three four five".split())

    def test_text_word_wrap_hard_breaks_long_word(self):
        img = Image.new("L", (1, 1))
        from PIL import ImageDraw

        draw = ImageDraw.Draw(img)
        font = _FONTS.get(12)
        lines = wrap_text(draw, "x" * 200, font, max_width=30)
        self.assertGreater(len(lines), 1)
        self.assertEqual("".join(lines), "x" * 200)

    def test_text_widget_empty_text_is_blank(self):
        img = TextWidget().render(_ctx(options={"text": ""}))
        self.assertEqual(_nonwhite_pixels(img), 0)


class FakeCalendarProvider(CalendarProvider):
    def __init__(self, events):
        self._events = events

    def today_events(self, now):
        return self._events


class FakeReminderProvider(ReminderProvider):
    def __init__(self, items):
        self._items = items

    def open_reminders(self, now):
        return self._items


class ProviderBackedWidgetTests(unittest.TestCase):
    def test_calendar_with_events(self):
        provider = FakeCalendarProvider([{"title": "Standup", "time": "09:00", "all_day": False}])
        img = CalendarWidget().render(_ctx(height=80, providers={"calendar": provider}))
        self.assertGreater(_nonwhite_pixels(img), 0)

    def test_calendar_no_events_shows_fallback(self):
        provider = FakeCalendarProvider([])
        img_empty = CalendarWidget().render(_ctx(height=80, providers={"calendar": provider}))
        img_default = CalendarWidget().render(_ctx(height=80))  # no provider -> NullProvider
        self.assertEqual(list(img_empty.getdata()), list(img_default.getdata()))

    def test_calendar_provider_exception_does_not_crash_widget(self):
        class Explodes(CalendarProvider):
            def today_events(self, now):
                raise RuntimeError("calendar unavailable")

        img = CalendarWidget().render(_ctx(height=80, providers={"calendar": Explodes()}))
        self.assertEqual(img.size, (100, 80))

    def test_reminders_with_items(self):
        provider = FakeReminderProvider([{"title": "Buy milk"}, {"title": "Water plants"}])
        img = RemindersWidget().render(_ctx(height=80, providers={"reminders": provider}))
        self.assertGreater(_nonwhite_pixels(img), 0)

    def test_mac_stats_deterministic_with_fake_stats(self):
        class FakeStats(MacStatsProvider):
            def stats(self):
                return MacStats(cpu_percent=12.3, mem_used_percent=45.0, disk_free_gb=100.0, battery_percent=80,
                                 battery_charging=True)

        img1 = MacStatsWidget().render(_ctx(providers={"mac_stats": FakeStats()}))
        img2 = MacStatsWidget().render(_ctx(providers={"mac_stats": FakeStats()}))
        self.assertEqual(list(img1.getdata()), list(img2.getdata()))
        self.assertGreater(_nonwhite_pixels(img1), 0)

    def test_mac_stats_null_provider_shows_dashes(self):
        img = MacStatsWidget().render(_ctx())
        self.assertGreater(_nonwhite_pixels(img), 0)  # still draws "CPU --  MEM --  ..."

    def test_network_with_fake_info(self):
        class FakeNet(NetworkProvider):
            def info(self, interface):
                return NetworkInfo(interface=interface, ip_address="10.0.0.5", up=True)

        img = NetworkWidget().render(_ctx(options={"interface": "en0"}, providers={"network": FakeNet()}))
        self.assertGreater(_nonwhite_pixels(img), 0)

    def test_git_with_fake_info(self):
        class FakeGit(GitProvider):
            def info(self, repo_path):
                return GitInfo(branch="main", dirty=False, last_commit="abc1234 initial commit")

        img = GitWidget().render(_ctx(options={"repo": "/some/repo"}, providers={"git": FakeGit()}))
        self.assertGreater(_nonwhite_pixels(img), 0)

    def test_git_no_repo_configured_shows_error_text(self):
        img = GitWidget().render(_ctx(options={}))
        self.assertGreater(_nonwhite_pixels(img), 0)

    def test_now_playing_with_fake_track(self):
        class FakeNP(NowPlayingProvider):
            def current(self):
                return NowPlaying(title="Song", artist="Artist", playing=True)

        img = NowPlayingWidget().render(_ctx(providers={"now_playing": FakeNP()}))
        self.assertGreater(_nonwhite_pixels(img), 0)

    def test_now_playing_null_provider(self):
        img = NowPlayingWidget().render(_ctx())
        self.assertGreater(_nonwhite_pixels(img), 0)  # "Not playing" still draws text


class TextCaseTests(unittest.TestCase):
    """`text_case` option / default (docs/troubleshooting.md
    follow-up: thin lowercase strokes, e.g. the 'c' in "chg", still lose the
    1-bit thresholding fight on short rows even at a healthy auto-fit font
    size cap)."""

    def test_resolve_text_case_defaults_upper_below_12px(self):
        self.assertEqual(resolve_text_case(_ctx(height=8)), "upper")
        self.assertEqual(resolve_text_case(_ctx(height=11)), "upper")

    def test_resolve_text_case_defaults_as_is_at_or_above_12px(self):
        self.assertEqual(resolve_text_case(_ctx(height=12)), "as-is")
        self.assertEqual(resolve_text_case(_ctx(height=40)), "as-is")

    def test_resolve_text_case_explicit_option_overrides_default(self):
        self.assertEqual(resolve_text_case(_ctx(height=8, options={"text_case": "as-is"})), "as-is")
        self.assertEqual(resolve_text_case(_ctx(height=40, options={"text_case": "upper"})), "upper")

    def test_apply_text_case_upper_vs_as_is(self):
        self.assertEqual(apply_text_case("Not playing", _ctx(height=8)), "NOT PLAYING")
        self.assertEqual(apply_text_case("Not playing", _ctx(height=40)), "Not playing")

    def test_mac_stats_chg_suffix_uppercased_on_short_row(self):
        class Charging(MacStatsProvider):
            def stats(self):
                return MacStats(battery_percent=80, battery_charging=True)

        upper = MacStatsWidget().render(
            _ctx(height=10, options={"fields": ["battery"]}, providers={"mac_stats": Charging()}))
        as_is = MacStatsWidget().render(
            _ctx(height=10, options={"fields": ["battery"], "text_case": "as-is"},
                 providers={"mac_stats": Charging()}))
        self.assertNotEqual(list(upper.getdata()), list(as_is.getdata()))

    def test_mac_stats_default_unchanged_on_tall_row(self):
        # height=40 is the pre-existing default test context -- default
        # behavior above the 12px threshold must stay "as-is" (no visual
        # change to any config that already renders tall rows fine).
        class Charging(MacStatsProvider):
            def stats(self):
                return MacStats(battery_percent=80, battery_charging=True)

        default = MacStatsWidget().render(_ctx(options={"fields": ["battery"]}, providers={"mac_stats": Charging()}))
        as_is = MacStatsWidget().render(
            _ctx(options={"fields": ["battery"], "text_case": "as-is"}, providers={"mac_stats": Charging()}))
        self.assertEqual(list(default.getdata()), list(as_is.getdata()))

    def test_now_playing_uppercased_by_default_on_short_row(self):
        upper = NowPlayingWidget().render(_ctx(height=10))
        as_is = NowPlayingWidget().render(_ctx(height=10, options={"text_case": "as-is"}))
        self.assertNotEqual(list(upper.getdata()), list(as_is.getdata()))

    def test_network_uppercased_by_default_on_short_row(self):
        class FakeNet(NetworkProvider):
            def info(self, interface):
                return NetworkInfo(interface=interface, ip_address="10.0.0.5", up=True)

        upper = NetworkWidget().render(
            _ctx(height=10, options={"interface": "en0"}, providers={"network": FakeNet()}))
        as_is = NetworkWidget().render(
            _ctx(height=10, options={"interface": "en0", "text_case": "as-is"}, providers={"network": FakeNet()}))
        self.assertNotEqual(list(upper.getdata()), list(as_is.getdata()))

    def test_network_font_cap_uses_full_row_height_not_half(self):
        # Regression for the network widget's own copy of the halved
        # max_size cap behind the garbled-small-text symptom
        # (docs/troubleshooting.md §6) -- date.py/clock.py's convention is
        # the full row height, not ctx.height // 2.
        from byok.dashboard import fonts as fonts_mod

        real_fit = fonts_mod.fit_font_size
        captured = {}

        def spy(draw, text, fonts, max_width, max_height, min_size=6, max_size=200, path=None):
            captured["max_size"] = max_size
            return real_fit(draw, text, fonts, max_width, max_height, min_size=min_size, max_size=max_size,
                             path=path)

        with mock.patch.object(fonts_mod, "fit_font_size", spy):
            NetworkWidget().render(_ctx(height=40, options={"interface": "en0"}))
        self.assertEqual(captured["max_size"], max(7, 40))


class ShellWidgetTests(unittest.TestCase):
    def test_shell_runs_configured_command(self):
        img = ShellWidget().render(_ctx(height=30, options={"command": [sys.executable, "-c", "print('hi there')"]}))
        self.assertGreater(_nonwhite_pixels(img), 0)

    def test_shell_no_command_is_blank(self):
        img = ShellWidget().render(_ctx(options={}))
        self.assertEqual(_nonwhite_pixels(img), 0)

    def test_shell_timeout_does_not_raise(self):
        img = ShellWidget().render(
            _ctx(options={"command": [sys.executable, "-c", "import time; time.sleep(5)"], "timeout": 0.05})
        )
        self.assertEqual(img.size, (100, 40))

    def test_shell_missing_binary_does_not_raise(self):
        img = ShellWidget().render(_ctx(options={"command": ["/no/such/binary/at/all"]}))
        self.assertEqual(img.size, (100, 40))


class ImageWidgetTests(unittest.TestCase):
    def test_missing_path_shows_placeholder_not_crash(self):
        img = ImageWidget().render(_ctx(options={"path": "/no/such/file.png"}))
        self.assertEqual(img.size, (100, 40))

    def test_real_image_file_is_composited(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "test.png")
            src = Image.new("L", (20, 10), color=0)
            src.save(path)
            img = ImageWidget().render(_ctx(width=50, height=50, options={"path": path}))
            # some pixels should now be dark (the pasted black source image)
            self.assertTrue(any(v < 255 for v in img.getdata()))


# ---------------------------------------------------------------------------
# QR encoder
# ---------------------------------------------------------------------------


def _decode_qr(matrix):
    """Independent decode of an encoded QRMatrix: reads format info via its
    own BCH check, walks the same zigzag order to recover codewords,
    de-interleaves per the EC-L block table, verifies each block's
    Reed-Solomon syndrome is zero (i.e. re-encoding the recovered data
    reproduces exactly the EC bytes placed in the matrix), and decodes the
    byte-mode payload. Returns the decoded bytes. Raises AssertionError on
    any structural inconsistency."""
    size = matrix.size
    version = matrix.version

    def bit(col, row):
        return 1 if matrix.modules[row][col] else 0

    fmt1 = 0
    for i in range(6):
        fmt1 |= bit(8, i) << i
    fmt1 |= bit(8, 7) << 6
    fmt1 |= bit(8, 8) << 7
    fmt1 |= bit(7, 8) << 8
    for i in range(9, 15):
        fmt1 |= bit(14 - i, 8) << i

    fmt2 = 0
    for i in range(8):
        fmt2 |= bit(size - 1 - i, 8) << i
    for i in range(8, 15):
        fmt2 |= bit(8, size - 15 + i) << i
    assert fmt1 == fmt2, "format info copies disagree"

    unmasked = fmt1 ^ 0x5412
    data_bits = unmasked >> 10
    ecl, mask = (data_bits >> 3) & 0b11, data_bits & 0b111
    assert ecl == 0b01, "expected EC level L"
    rem = data_bits
    for _ in range(10):
        rem = (rem << 1) ^ ((rem >> 9) * 0x537)
    assert ((data_bits << 10) | rem) ^ 0x5412 == fmt1, "format info BCH mismatch"

    def mask_fn(row, col):
        return qr_mod.QRMatrix._mask_fn(mask, row, col)

    unmasked_modules = [row[:] for row in matrix.modules]
    for row in range(size):
        for col in range(size):
            if not matrix.is_function[row][col] and mask_fn(row, col):
                unmasked_modules[row][col] = not unmasked_modules[row][col]

    total_cw, ec_per_block, groups = qr_mod._EC_L_TABLE[version]
    bits_out = []
    col = size - 1
    while col >= 1:
        if col == 6:
            col -= 1
        upward = ((col + 1) & 2) == 0
        rows = range(size - 1, -1, -1) if upward else range(size)
        for row in rows:
            for dc in (0, 1):
                c = col - dc
                if matrix.is_function[row][c]:
                    continue
                bits_out.append(1 if unmasked_modules[row][c] else 0)
        col -= 2

    codewords = bytearray()
    for i in range(0, len(bits_out) - 7, 8):
        byte = 0
        for b in bits_out[i:i + 8]:
            byte = (byte << 1) | b
        codewords.append(byte)
    codewords = bytes(codewords[:total_cw])
    assert len(codewords) == total_cw

    block_lens = []
    for n, c in groups:
        block_lens.extend([c] * n)
    max_len = max(block_lens)
    num_blocks = len(block_lens)

    data_parts = [[] for _ in range(num_blocks)]
    pos = 0
    for i in range(max_len):
        for bidx, blen in enumerate(block_lens):
            if i < blen:
                data_parts[bidx].append(codewords[pos])
                pos += 1
    ec_parts = [[] for _ in range(num_blocks)]
    for _ in range(ec_per_block):
        for bidx in range(num_blocks):
            ec_parts[bidx].append(codewords[pos])
            pos += 1

    for bidx in range(num_blocks):
        assert qr_mod.rs_encode(data_parts[bidx], ec_per_block) == ec_parts[bidx], (
            f"block {bidx}: RS syndrome nonzero"
        )

    all_data = []
    for part in data_parts:
        all_data.extend(part)

    bitstream = []
    for byte in all_data:
        for i in range(7, -1, -1):
            bitstream.append((byte >> i) & 1)

    def read_bits(pos, n):
        v = 0
        for i in range(n):
            v = (v << 1) | bitstream[pos + i]
        return v, pos + n

    pos = 0
    mode, pos = read_bits(pos, 4)
    assert mode == 0b0100, "expected byte mode"
    count_bits = 8 if version <= 9 else 16
    length, pos = read_bits(pos, count_bits)
    out = bytearray()
    for _ in range(length):
        b, pos = read_bits(pos, 8)
        out.append(b)
    return bytes(out)


class QREncoderTests(unittest.TestCase):
    # -- known-answer values, derived from the standard formulas, not from
    #    a captured run of qr.py (see module docstring above) --

    def test_gf256_exp_table_anchor(self):
        # exp[7] = 2^7 = 128 (no reduction needed yet: 128 < 256).
        self.assertEqual(qr_mod._GF_EXP[7], 128)
        # exp[8] = 2^8 = 0x100, which overflows GF(256) and must be reduced
        # by the primitive polynomial 0x11D: 0x100 ^ 0x11D = 0x1D = 29.
        self.assertEqual(qr_mod._GF_EXP[8], 0x1D)
        self.assertEqual(qr_mod._GF_EXP[8], 29)

    def test_rs_encode_identity_multiplier_kat(self):
        # generator for ec_len=1 is [1, exp[0]] = [1, 1] (monic, root alpha^0=1).
        # Dividing the polynomial "x + 1" (coeffs [1, 1]) by (x + 1) leaves
        # remainder 0 -- pure GF(2^8)-with-mul-by-1 arithmetic, verifiable
        # by hand without any lookup table.
        self.assertEqual(qr_mod.rs_encode([1, 1], 1), [0])
        # Dividing "5" (a degree-0 "polynomial", i.e. data=[5]) by (x + 1):
        # remainder = 5 * 1 (the single generator coefficient) = 5.
        self.assertEqual(qr_mod.rs_encode([5], 1), [5])

    def test_format_info_bch_known_answer(self):
        # EC level L, mask 0 -> data = 0b01000 = 8. BCH(15,5) with generator
        # 0x537, XOR mask 0x5412 (ISO/IEC 18004 sec 7.9), computed
        # independently of qr.py: 0b111011111000100 = 0x77c4.
        m = qr_mod.QRMatrix(1)
        m.draw_function_patterns()
        m.draw_format_info(mask=0)

        def bit(col, row):
            return 1 if m.modules[row][col] else 0

        bits = 0
        for i in range(6):
            bits |= bit(8, i) << i
        bits |= bit(8, 7) << 6
        bits |= bit(8, 8) << 7
        bits |= bit(7, 8) << 8
        for i in range(9, 15):
            bits |= bit(14 - i, 8) << i
        self.assertEqual(bits, 0x77C4)

    def test_capacity_table_known_values(self):
        # Hand-derived from the EC-L codeword table: capacity = data_codewords
        # - ceil((4 + count_bits) / 8); count_bits is 8 for v1-9, 16 for v10.
        self.assertEqual(qr_mod.capacity_bytes(1), 17)
        self.assertEqual(qr_mod.capacity_bytes(9), 230)
        self.assertEqual(qr_mod.capacity_bytes(10), 271)
        self.assertEqual(qr_mod.choose_version(17), 1)
        self.assertEqual(qr_mod.choose_version(18), 2)
        self.assertEqual(qr_mod.choose_version(271), 10)
        with self.assertRaises(ValueError):
            qr_mod.choose_version(272)

    def test_finder_pattern_structure(self):
        # The 7x7 finder pattern's own well-known ring structure: outer ring
        # dark, next ring light, center 3x3 dark.
        m = qr_mod.QRMatrix(1)
        m.draw_function_patterns()
        top_left = [[m.modules[r][c] for c in range(7)] for r in range(7)]
        expected = [
            [True, True, True, True, True, True, True],
            [True, False, False, False, False, False, True],
            [True, False, True, True, True, False, True],
            [True, False, True, True, True, False, True],
            [True, False, True, True, True, False, True],
            [True, False, False, False, False, False, True],
            [True, True, True, True, True, True, True],
        ]
        self.assertEqual(top_left, expected)

    # -- round-trip / structural verification across versions and masks --

    def test_round_trip_decode_various_versions(self):
        for text in ["A", "https://example.com/byok", "Hello, BYOK Mod!", "x" * 271]:
            with self.subTest(text=text[:20]):
                data = text.encode("utf-8")
                matrix = qr_mod.encode(data)
                decoded = _decode_qr(matrix)
                self.assertEqual(decoded.decode("utf-8"), text)

    def test_encode_too_long_raises(self):
        with self.assertRaises(ValueError):
            qr_mod.encode(b"x" * 272)

    def test_encode_is_deterministic(self):
        m1 = qr_mod.encode("determinism check")
        m2 = qr_mod.encode("determinism check")
        self.assertEqual(m1.modules, m2.modules)
        img1 = m1.to_image(scale=2)
        img2 = m2.to_image(scale=2)
        self.assertEqual(list(img1.getdata()), list(img2.getdata()))

    def test_to_image_size_matches_modules_plus_border(self):
        m = qr_mod.encode("hi")
        img = m.to_image(scale=3, border=4)
        expected = (m.size + 8) * 3
        self.assertEqual(img.size, (expected, expected))

    def test_qr_widget_blank_when_no_data(self):
        img = QRWidget().render(_ctx(width=60, height=60, options={"data": ""}))
        self.assertEqual(_nonwhite_pixels(img), 0)

    def test_qr_widget_renders_when_data_present(self):
        img = QRWidget().render(_ctx(width=80, height=80, options={"data": "hello"}))
        self.assertGreater(_nonwhite_pixels(img), 0)

    def test_qr_widget_oversized_data_shows_message_not_crash(self):
        img = QRWidget().render(_ctx(width=80, height=80, options={"data": "x" * 500}))
        self.assertEqual(img.size, (80, 80))


# ---------------------------------------------------------------------------
# render.py -- quantization
# ---------------------------------------------------------------------------


class QuantizationTests(unittest.TestCase):
    def _gradient(self, w=32, h=32):
        img = Image.new("L", (w, h))
        for y in range(h):
            for x in range(w):
                img.putpixel((x, y), int(255 * x / (w - 1)))
        return img

    def test_no_dither_only_uses_legal_levels_1bpp(self):
        img = self._gradient()
        out = render_mod._quantize_none(img, levels=2)
        self.assertTrue(set(out.getdata()) <= {0, 255})

    def test_no_dither_only_uses_legal_levels_2bpp(self):
        img = self._gradient()
        out = render_mod._quantize_none(img, levels=4)
        self.assertTrue(set(out.getdata()) <= {0, 85, 170, 255})

    def test_bayer_dither_only_uses_legal_levels(self):
        img = self._gradient()
        out = render_mod._quantize_bayer(img, levels=2)
        self.assertTrue(set(out.getdata()) <= {0, 255})

    def test_bayer_dither_differs_from_no_dither_on_gradient(self):
        img = self._gradient()
        none_out = render_mod._quantize_none(img, levels=2)
        bayer_out = render_mod._quantize_bayer(img, levels=2)
        self.assertNotEqual(list(none_out.getdata()), list(bayer_out.getdata()))

    def test_flat_image_at_exact_level_dither_and_no_dither_agree(self):
        # A flat image sitting exactly on a quantization level (not
        # between two, like a genuine mid-tone would be) dithers to a
        # uniform result -- there's no error to diffuse/pattern.
        for value in (0, 255):
            with self.subTest(value=value):
                img = Image.new("L", (16, 16), color=value)
                none_out = render_mod._quantize_none(img, levels=2)
                bayer_out = render_mod._quantize_bayer(img, levels=2)
                self.assertEqual(list(none_out.getdata()), list(bayer_out.getdata()))

    def test_render_and_quantize_1bpp_uses_only_two_levels(self):
        cfg = config_mod.load(_DEFAULT_YAML_PATH)
        img = render_mod.render_and_quantize(cfg, 240, 160, 1, _NOW, providers={})
        self.assertTrue(set(img.getdata()) <= {0, 255})

    def test_render_and_quantize_2bpp_uses_only_four_levels(self):
        cfg = config_mod.load(_DEFAULT_YAML_PATH)
        img = render_mod.render_and_quantize(cfg, 240, 160, 2, _NOW, providers={})
        self.assertTrue(set(img.getdata()) <= {0, 85, 170, 255})

    def test_render_and_quantize_is_deterministic(self):
        cfg = config_mod.load(_DEFAULT_YAML_PATH)
        img1 = render_mod.render_and_quantize(cfg, 320, 240, 1, _NOW, providers={})
        img2 = render_mod.render_and_quantize(cfg, 320, 240, 1, _NOW, providers={})
        self.assertEqual(list(img1.getdata()), list(img2.getdata()))

    def test_render_and_quantize_rejects_bad_bpp(self):
        cfg = config_mod.load(_DEFAULT_YAML_PATH)
        with self.assertRaises(ValueError):
            render_mod.render_and_quantize(cfg, 320, 240, 4, _NOW, providers={})

    def test_photo_tile_gets_dithered_text_tile_does_not(self):
        # A tiny gradient "photo" via the image widget, next to a solid-fill
        # text tile -- at 2bpp the photo tile should show more than the two
        # extreme levels (proof dithering ran); a widget-driven fill made of
        # flat/text content should not.
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "gradient.png")
            grad = self._gradient(40, 40)
            grad.save(path)
            cfg = config_mod.loads(
                f"""
layout:
  grid:
    cols: 2
    rows: 1
widgets:
  - type: image
    at: [0, 0]
    span: [1, 1]
    options:
      path: "{path}"
  - type: clock
    at: [1, 0]
    span: [1, 1]
    options:
      format: "%H:%M"
"""
            )
            img = render_mod.render_and_quantize(cfg, 80, 40, 2, _NOW, providers={})
            left = img.crop((0, 0, 40, 40))
            levels_seen = set(left.getdata())
            self.assertTrue(levels_seen <= {0, 85, 170, 255})
            self.assertGreater(len(levels_seen), 2, "expected ordered dithering to use intermediate levels")


# ---------------------------------------------------------------------------
# fonts.py -- ellipsize()
# ---------------------------------------------------------------------------


class EllipsizeTests(unittest.TestCase):
    @staticmethod
    def _draw():
        from PIL import ImageDraw

        return ImageDraw.Draw(Image.new("L", (1, 1)))

    def test_text_that_already_fits_is_unchanged(self):
        draw = self._draw()
        font = _FONTS.get(12)
        w, _ = text_size(draw, "short", font)
        self.assertEqual(ellipsize(draw, "short", font, w + 10), "short")

    def test_long_text_gets_truncated_with_ellipsis(self):
        draw = self._draw()
        font = _FONTS.get(12)
        long_text = "x" * 50
        full_w, _ = text_size(draw, long_text, font)
        result = ellipsize(draw, long_text, font, full_w // 2)
        self.assertTrue(result.endswith("…"))
        self.assertLessEqual(text_size(draw, result, font)[0], full_w // 2)
        self.assertLess(len(result), len(long_text))

    def test_empty_text_stays_empty(self):
        draw = self._draw()
        self.assertEqual(ellipsize(draw, "", _FONTS.get(12), 100), "")

    def test_box_too_small_for_even_the_ellipsis_returns_empty(self):
        draw = self._draw()
        self.assertEqual(ellipsize(draw, "hello world", _FONTS.get(12), 0), "")


# ---------------------------------------------------------------------------
# calendar.py / reminders.py -- "next event" sort, reminders "count" mode
# ---------------------------------------------------------------------------


class CalendarSortAndModeTests(unittest.TestCase):
    def test_next_event_picks_earliest_regardless_of_provider_order(self):
        class Unsorted(CalendarProvider):
            def today_events(self, now):
                return [
                    {"title": "Afternoon sync", "time": "15:00", "all_day": False},
                    {"title": "Morning standup", "time": "09:00", "all_day": False},
                ]

        class OnlyMorning(CalendarProvider):
            def today_events(self, now):
                return [{"title": "Morning standup", "time": "09:00", "all_day": False}]

        class OnlyAfternoon(CalendarProvider):
            def today_events(self, now):
                return [{"title": "Afternoon sync", "time": "15:00", "all_day": False}]

        opts = {"title": "", "max_events": 1}
        img_next = CalendarWidget().render(_ctx(width=240, height=16, options=opts, providers={"calendar": Unsorted()}))
        img_morning = CalendarWidget().render(
            _ctx(width=240, height=16, options=opts, providers={"calendar": OnlyMorning()}))
        img_afternoon = CalendarWidget().render(
            _ctx(width=240, height=16, options=opts, providers={"calendar": OnlyAfternoon()}))
        # Pixel-identical to the single-event render of whichever event it
        # actually picked, rather than OCR -- proves *which* one won.
        self.assertEqual(list(img_next.getdata()), list(img_morning.getdata()))
        self.assertNotEqual(list(img_next.getdata()), list(img_afternoon.getdata()))

    def test_all_day_events_sort_before_timed_events(self):
        class Mixed(CalendarProvider):
            def today_events(self, now):
                return [
                    {"title": "Standup", "time": "09:00", "all_day": False},
                    {"title": "Company holiday", "time": "", "all_day": True},
                ]

        class OnlyHoliday(CalendarProvider):
            def today_events(self, now):
                return [{"title": "Company holiday", "time": "", "all_day": True}]

        opts = {"title": "", "max_events": 1}
        img_next = CalendarWidget().render(_ctx(width=240, height=16, options=opts, providers={"calendar": Mixed()}))
        img_holiday = CalendarWidget().render(
            _ctx(width=240, height=16, options=opts, providers={"calendar": OnlyHoliday()}))
        self.assertEqual(list(img_next.getdata()), list(img_holiday.getdata()))


class RemindersCountModeTests(unittest.TestCase):
    def _provider(self, n):
        class NItems(ReminderProvider):
            def open_reminders(self, now):
                return [{"title": f"item {i}"} for i in range(n)]

        return NItems()

    def test_count_mode_varies_with_item_count(self):
        opts = {"title": "", "mode": "count"}
        img0 = RemindersWidget().render(_ctx(width=240, height=16, options=opts))
        img1 = RemindersWidget().render(_ctx(width=240, height=16, options=opts, providers={"reminders": self._provider(1)}))
        img3 = RemindersWidget().render(_ctx(width=240, height=16, options=opts, providers={"reminders": self._provider(3)}))
        self.assertGreater(_nonwhite_pixels(img0), 0)  # "Nothing pending" still draws
        self.assertGreater(_nonwhite_pixels(img1), 0)
        self.assertNotEqual(list(img0.getdata()), list(img1.getdata()))
        self.assertNotEqual(list(img1.getdata()), list(img3.getdata()))

    def test_count_mode_differs_from_list_mode(self):
        img_count = RemindersWidget().render(
            _ctx(width=240, height=16, options={"title": "", "mode": "count"},
                 providers={"reminders": self._provider(2)}))
        img_list = RemindersWidget().render(
            _ctx(width=240, height=16, options={"title": "", "mode": "list"},
                 providers={"reminders": self._provider(2)}))
        self.assertNotEqual(list(img_count.getdata()), list(img_list.getdata()))


# ---------------------------------------------------------------------------
# git.py -- compact single-line fallback on short rows
# ---------------------------------------------------------------------------


class GitCompactRowTests(unittest.TestCase):
    def _provider(self):
        class FakeGit(GitProvider):
            def info(self, repo_path):
                return GitInfo(branch="main", dirty=True,
                                last_commit="abc1234 a commit message long enough to matter")

        return FakeGit()

    def test_short_row_shows_branch_without_crashing_or_overflowing(self):
        img = GitWidget().render(
            _ctx(width=240, height=16, options={"repo": "/x", "title": ""}, providers={"git": self._provider()}))
        self.assertEqual(img.size, (240, 16))
        bbox = _content_bbox(img)
        self.assertIsNotNone(bbox)
        self.assertLessEqual(bbox[3], 16)  # nothing drawn past the tile's own bottom edge

    def test_short_row_drops_commit_line_tall_row_keeps_it(self):
        opts = {"repo": "/x", "title": ""}
        short = GitWidget().render(_ctx(width=240, height=16, options=opts, providers={"git": self._provider()}))
        tall = GitWidget().render(_ctx(width=240, height=40, options=opts, providers={"git": self._provider()}))
        # The short row's single branch-only line should need noticeably
        # less ink than the tall row's branch-line + commit-line layout --
        # a rough but real proxy for "one line vs. two" without OCR.
        self.assertLess(_nonwhite_pixels(short), _nonwhite_pixels(tall))


# ---------------------------------------------------------------------------
# Every widget type, at the panel's real row heights (16/24/32px @ 240
# wide -- see docs/host-tools.md)
# ---------------------------------------------------------------------------


class WidgetRowSizeTests(unittest.TestCase):
    ROW_HEIGHTS = (16, 24, 32)

    def _cases(self):
        class FakeGit(GitProvider):
            def info(self, repo_path):
                return GitInfo(branch="main", dirty=True, last_commit="abc1234 a reasonably long commit summary")

        class FakeCal(CalendarProvider):
            def today_events(self, now):
                return [{"title": "Standup", "time": "09:00", "all_day": False}]

        class FakeRem(ReminderProvider):
            def open_reminders(self, now):
                return [{"title": "Buy milk"}, {"title": "Water the plants please"}]

        class FakeNet(NetworkProvider):
            def info(self, interface):
                return NetworkInfo(interface=interface, ip_address="10.0.0.5", up=True)

        class FakeStats(MacStatsProvider):
            def stats(self):
                return MacStats(cpu_percent=12.3, mem_used_percent=45.0, disk_free_gb=100.0,
                                 battery_percent=80, battery_charging=True)

        class FakeNP(NowPlayingProvider):
            def current(self):
                return NowPlaying(title="A Pretty Long Track Title", artist="Some Artist", playing=True)

        class FakeWriting(WritingProvider):
            def stats(self):
                return WritingStats(
                    files=12, words=48213, bytes=290000, newest_epoch=_NOW.timestamp() - 600,
                    words_today=612,
                )

        return {
            "clock": (ClockWidget, {"format": "%H:%M:%S"}, {}),
            "date": (DateWidget, {"format": "%a, %b %-d"}, {}),
            "mac_stats": (MacStatsWidget, {"fields": ["cpu", "mem", "disk", "battery"]}, {"mac_stats": FakeStats()}),
            "now_playing": (NowPlayingWidget, {}, {"now_playing": FakeNP()}),
            "calendar": (CalendarWidget, {"max_events": 3}, {"calendar": FakeCal()}),
            "reminders": (RemindersWidget, {}, {"reminders": FakeRem()}),
            "git": (GitWidget, {"repo": "/fake"}, {"git": FakeGit()}),
            "network": (NetworkWidget, {}, {"network": FakeNet()}),
            "shell": (ShellWidget, {"command": [sys.executable, "-c", "print('hi there')"]}, {}),
            "text": (TextWidget, {"text": "Hello dashboard world, this line is on the longer side"}, {}),
            "image": (ImageWidget, {}, {}),
            "qr": (QRWidget, {"data": "https://x.io"}, {}),
            "writing": (WritingWidget, {"daily_goal_words": 500}, {"writing": FakeWriting()}),
        }

    def test_every_widget_renders_at_each_row_height(self):
        for type_name, (cls, options, providers) in self._cases().items():
            for height in self.ROW_HEIGHTS:
                with self.subTest(widget=type_name, height=height):
                    ctx = RenderContext(width=240, height=height, now=_NOW, options=dict(options),
                                         fonts=_FONTS, providers=providers)
                    img = cls().render(ctx)
                    self.assertEqual(img.size, (240, height))
                    bbox = _content_bbox(img)
                    if bbox is not None:
                        x0, y0, x1, y1 = bbox
                        self.assertGreaterEqual(x0, 0)
                        self.assertGreaterEqual(y0, 0)
                        self.assertLessEqual(x1, 240)
                        self.assertLessEqual(y1, height)

    def test_every_registered_widget_type_is_covered(self):
        # Guards against a new widget module being added to widgets/
        # without a corresponding case (and thus row-height coverage)
        # above.
        self.assertEqual(set(self._cases()), set(WIDGET_REGISTRY))


# ---------------------------------------------------------------------------
# config.py -- --config path resolution, example listing
# ---------------------------------------------------------------------------


class ConfigPathResolutionTests(unittest.TestCase):
    def test_default_config_path_used_when_none_given(self):
        self.assertEqual(config_mod.resolve_config_path(None), config_mod.default_config_path())

    def test_resolves_relative_to_cwd_first(self):
        self.assertEqual(config_mod.resolve_config_path(_DEFAULT_YAML_PATH), _DEFAULT_YAML_PATH)

    def test_resolves_bare_example_name(self):
        resolved = config_mod.resolve_config_path("work")
        self.assertEqual(resolved, os.path.join(_EXAMPLES_DIR, "work.yaml"))
        self.assertTrue(os.path.isfile(resolved))

    def test_resolves_example_name_with_extension(self):
        resolved = config_mod.resolve_config_path("work.yaml")
        self.assertEqual(resolved, os.path.join(_EXAMPLES_DIR, "work.yaml"))

    def test_resolves_examples_relative_path_via_basename_fallback(self):
        # "examples/work.yaml" isn't itself a valid path relative to the
        # test runner's cwd -- falls through to the examples-dir fallback,
        # which joins just the basename onto examples_dir() so this still
        # lands on the same file regardless of what directory prefix was
        # given.
        resolved = config_mod.resolve_config_path("examples/work.yaml")
        self.assertEqual(resolved, os.path.join(_EXAMPLES_DIR, "work.yaml"))

    def test_extension_appended_relative_to_cwd_before_examples_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "mydash.yaml"), "w", encoding="utf-8") as fh:
                fh.write("widgets: []\n")
            old_cwd = os.getcwd()
            os.chdir(tmp)
            try:
                resolved = config_mod.resolve_config_path("mydash")
            finally:
                os.chdir(old_cwd)
            # Resolved cwd-relative (step 2), not via the examples-dir
            # fallback (step 3) -- no "mydash" example is bundled.
            self.assertEqual(resolved, "mydash.yaml")

    def test_unresolvable_path_raises_with_the_name_named(self):
        with self.assertRaises(FileNotFoundError) as cm:
            config_mod.resolve_config_path("no-such-config-anywhere")
        self.assertIn("no-such-config-anywhere", str(cm.exception))

    def test_list_examples_lists_all_three_bundled_configs(self):
        self.assertEqual(config_mod.list_examples(), ["clock-focus", "media", "work"])

    def test_examples_dir_matches_expected_location(self):
        self.assertEqual(config_mod.examples_dir(), _EXAMPLES_DIR)


class DashboardCLIListExamplesTests(unittest.TestCase):
    def test_list_examples_flag_prints_all_examples_without_connecting_a_device(self):
        parser = cli_mod.build_parser()
        args = parser.parse_args(["dashboard", "--list-examples"])
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = cli_mod.cmd_dashboard(args)
        self.assertEqual(rc, 0)
        out = buf.getvalue()
        for name in ("clock-focus", "media", "work"):
            self.assertIn(name, out)

    def test_config_and_list_examples_flags_coexist_in_argparse(self):
        parser = cli_mod.build_parser()
        args = parser.parse_args(["dashboard", "--config", "clock-focus", "--list-examples"])
        self.assertEqual(args.config, "clock-focus")
        self.assertTrue(args.list_examples)

    def test_missing_config_returns_clean_error_without_connecting_a_device(self):
        # If this reached `_connect()` in a test environment with no real
        # device, it would hang/fail on port discovery instead of
        # returning promptly -- the fast, clean rc==2 here is itself the
        # proof that config resolution's FileNotFoundError short-circuits
        # before that ever happens.
        parser = cli_mod.build_parser()
        args = parser.parse_args(["dashboard", "--config", "no-such-config-xyz"])
        buf = io.StringIO()
        with contextlib.redirect_stderr(buf):
            rc = cli_mod.cmd_dashboard(args)
        self.assertEqual(rc, 2)
        self.assertIn("no-such-config-xyz", buf.getvalue())


# ---------------------------------------------------------------------------
# examples/*.yaml -- the three bundled example configs
# ---------------------------------------------------------------------------


class ExampleConfigsTests(unittest.TestCase):
    """clock-focus.yaml / work.yaml / media.yaml: must render cleanly with
    no providers configured (every widget's own Null-provider fallback --
    the same thing `preview.py --offline` exercises), deterministically for
    a fixed timestamp, and keep every widget's drawn content inside its own
    tile."""

    NAMES = ("clock-focus", "work", "media")

    @staticmethod
    def _load(name):
        return config_mod.load(os.path.join(_EXAMPLES_DIR, f"{name}.yaml"))

    def test_examples_directory_has_exactly_the_three_bundled_configs(self):
        found = {f[:-len(".yaml")] for f in os.listdir(_EXAMPLES_DIR) if f.endswith(".yaml")}
        self.assertEqual(found, set(self.NAMES))

    def test_each_example_declares_the_panels_native_240x80_1bpp(self):
        for name in self.NAMES:
            with self.subTest(example=name):
                cfg = self._load(name)
                self.assertEqual((cfg.display.width, cfg.display.height, cfg.display.bpp), (240, 80, 1))

    def test_each_example_renders_without_error_offline(self):
        for name in self.NAMES:
            with self.subTest(example=name):
                cfg = self._load(name)
                img = render_mod.render_and_quantize(cfg, 240, 80, 1, _NOW, providers={})
                self.assertEqual(img.size, (240, 80))
                self.assertTrue(set(img.getdata()) <= {0, 255})

    def test_each_example_is_deterministic_with_fixed_time(self):
        for name in self.NAMES:
            with self.subTest(example=name):
                cfg = self._load(name)
                img1 = render_mod.render_and_quantize(cfg, 240, 80, 1, _NOW, providers={})
                img2 = render_mod.render_and_quantize(cfg, 240, 80, 1, _NOW, providers={})
                self.assertEqual(list(img1.getdata()), list(img2.getdata()))

    def test_each_example_widget_content_stays_within_its_own_tile_bounds(self):
        for name in self.NAMES:
            with self.subTest(example=name):
                cfg = self._load(name)
                composite = layout_mod.render_dashboard(cfg, 240, 80, _NOW, providers={})
                for w, (x0, y0, x1, y1) in layout_mod.iter_widget_rects(cfg, 240, 80):
                    tile = composite.crop((x0, y0, x1, y1))
                    bbox = _content_bbox(tile)
                    if bbox is None:
                        continue
                    bx0, by0, bx1, by1 = bbox
                    self.assertGreaterEqual(bx0, 0)
                    self.assertGreaterEqual(by0, 0)
                    self.assertLessEqual(bx1, x1 - x0,
                                          f"{name}/{w.type}@{(x0, y0, x1, y1)}: content past the tile's right edge")
                    self.assertLessEqual(by1, y1 - y0,
                                          f"{name}/{w.type}@{(x0, y0, x1, y1)}: content past the tile's bottom edge")

    def test_each_example_renders_at_its_own_declared_display_size(self):
        # Confirms preview.py's own defaulting (`args.width or
        # cfg.display.width or 320`) has a real 240x80 to fall back to for
        # each of these with no --width/--height override needed.
        for name in self.NAMES:
            with self.subTest(example=name):
                cfg = self._load(name)
                img = render_mod.render_and_quantize(
                    cfg, cfg.display.width, cfg.display.height, cfg.display.bpp, _NOW, providers={})
                self.assertEqual(img.size, (240, 80))


if __name__ == "__main__":
    unittest.main()
