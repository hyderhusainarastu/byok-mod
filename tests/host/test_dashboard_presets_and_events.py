"""tests/host/test_dashboard_presets_and_events.py -- DashboardLoop's
device-event handling (PRESET_CHANGED, EVT_BUTTON) and notify-banner
overlay, all new in this release. Companion to test_dashboard_loop.py, which
covers the pre-existing render/send cycle these features are layered on
top of -- kept as a separate file rather than added there so the two
concerns (the render/send cycle vs. these three additions) stay easy to
find independently.
"""

from __future__ import annotations

import datetime
import os
import struct
import sys
import tempfile
import unittest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from PIL import Image  # noqa: E402

from byok import device as device_mod  # noqa: E402
from byok import notify_ipc  # noqa: E402
from byok import proto  # noqa: E402
from byok.dashboard import presets as presets_mod  # noqa: E402
from byok.dashboard.loop import DashboardLoop  # noqa: E402

from _fake_transport import connected_device  # noqa: E402

_NOW = datetime.datetime(2026, 9, 4, 14, 32, 7)


def _make_loop(device, **kwargs):
    entries = presets_mod.default_presets()
    cfg = presets_mod.load_preset_config(entries[0])  # "clock", index 0
    kwargs.setdefault("interval", 1.0)
    kwargs.setdefault("now_fn", lambda: _NOW)
    kwargs.setdefault("presets", entries)
    # Never let the suite touch the owner's live ~/.cache/byok/ files --
    # both default to a real path unless a test explicitly overrides them.
    kwargs.setdefault("lock_path", None)
    kwargs.setdefault("notify_path", None)  # opt out unless a test wants it
    return DashboardLoop(device, cfg, providers={}, **kwargs), entries


class PresetSwitchingTests(unittest.TestCase):
    def setUp(self):
        self.device, self.serials, _clock = connected_device()
        self.addCleanup(self.device.close)

    def test_preset_changed_event_switches_config_and_forces_full_frame(self):
        loop, entries = _make_loop(self.device)
        loop.force_preset(0)  # seed the initial index -- see force_preset()'s
        # own docstring: nothing infers it from `config` alone.
        loop.run_once()  # first frame is always full regardless

        media_index = [e.name for e in entries].index("media")
        self.serials["serial"].push_event(
            proto.Type.EVT_PRESET_CHANGED, struct.pack("<B", media_index)
        )
        stats = loop.run_once()

        self.assertEqual(loop.active_preset_name, "media")
        self.assertTrue(stats.full)  # switching presets must force a full frame

    def test_out_of_range_index_is_ignored(self):
        loop, entries = _make_loop(self.device)
        loop.force_preset(0)
        loop.run_once()

        self.serials["serial"].push_event(proto.Type.EVT_PRESET_CHANGED, struct.pack("<B", 999 & 0xFF))
        loop.run_once()  # must not raise

        self.assertEqual(loop.active_preset_name, "clock")  # unchanged

    def test_switching_to_the_already_active_preset_is_a_no_op(self):
        loop, entries = _make_loop(self.device)
        loop.force_preset(0)
        loop.run_once()  # first frame is always full regardless
        loop.force_preset(0)  # already showing index 0 ("clock") -- a no-op
        stats = loop.run_once()
        self.assertFalse(stats.full)  # no forced full frame from a no-op switch

    def test_force_preset_used_for_the_cli_preset_flag(self):
        loop, entries = _make_loop(self.device, presets=None)  # not preset-driven yet
        self.assertIsNone(loop.active_preset_name)
        loop.presets = entries
        loop.force_preset([e.name for e in entries].index("writing"))
        self.assertEqual(loop.active_preset_name, "writing")

    def test_locked_preset_ignores_preset_changed_but_keeps_active_preset_name(self):
        loop, entries = _make_loop(self.device)
        media_index = [e.name for e in entries].index("media")
        loop.force_preset(media_index)  # e.g. cli.py's --preset media
        loop.preset_switching_locked = True  # ... then locked, in that order
        loop.run_once()

        clock_index = [e.name for e in entries].index("clock")
        self.serials["serial"].push_event(proto.Type.EVT_PRESET_CHANGED, struct.pack("<B", clock_index))
        loop.run_once()

        # Ignored -- still pinned to "media" (and active_preset_name, what
        # media_transport's button gate reads, still reports it).
        self.assertEqual(loop.active_preset_name, "media")

    def test_no_presets_configured_means_preset_changed_is_ignored(self):
        loop, _entries = _make_loop(self.device, presets=None)
        original_config = loop.config
        self.serials["serial"].push_event(proto.Type.EVT_PRESET_CHANGED, struct.pack("<B", 0))
        loop.run_once()  # must not raise
        self.assertIs(loop.config, original_config)

    def test_evt_status_preset_index_bits_also_switch_the_preset(self):
        # docs/protocol.md §6.1/§6.4b: STATUS/EVT_STATUS's `flags` byte
        # carries the selected preset index in bits 4-6 as of v1.2 -- the
        # second of the two channels the protocol defines for this
        # ("STATUS / PRESET_CHANGED events").
        loop, entries = _make_loop(self.device)
        loop.force_preset(0)
        loop.run_once()

        media_index = [e.name for e in entries].index("media")
        status_flags = (media_index & 0x07) << 4  # bits 4-6, bit7 (menu open) clear
        status_payload = struct.pack(
            "<HBBBBBBIII",
            3700, 82, 1,           # battery_mv, battery_pct, charging
            1,                      # mode (HOST)
            200, 128,                # backlight, contrast
            status_flags,
            12345,                   # uptime_ms
            50000, 40000,             # free_heap, min_free_heap
        )
        self.serials["serial"].push_event(proto.Type.EVT_STATUS, status_payload)
        loop.run_once()

        self.assertEqual(loop.active_preset_name, "media")

    def test_evt_status_with_no_presets_configured_is_ignored(self):
        loop, _entries = _make_loop(self.device, presets=None)
        status_payload = struct.pack(
            "<HBBBBBBIII", 3700, 82, 1, 1, 200, 128, 0x30, 12345, 50000, 40000,
        )
        self.serials["serial"].push_event(proto.Type.EVT_STATUS, status_payload)
        loop.run_once()  # must not raise


class ButtonEventTests(unittest.TestCase):
    def setUp(self):
        self.device, self.serials, _clock = connected_device()
        self.addCleanup(self.device.close)

    def test_button_press_dispatches_to_on_button(self):
        received = []
        loop, _entries = _make_loop(self.device, on_button=received.append)
        loop.run_once()

        self.serials["serial"].push_event(
            proto.Type.EVT_BUTTON,
            struct.pack("<BBI", device_mod.BUTTON_UP, device_mod.BUTTON_STATE_PRESSED, 1234),
        )
        loop.run_once()

        self.assertEqual(len(received), 1)
        self.assertEqual(received[0].button, device_mod.BUTTON_UP)
        self.assertEqual(received[0].state, device_mod.BUTTON_STATE_PRESSED)
        self.assertEqual(received[0].t_ms, 1234)

    def test_no_on_button_configured_is_fine(self):
        loop, _entries = _make_loop(self.device, on_button=None)
        loop.run_once()
        self.serials["serial"].push_event(
            proto.Type.EVT_BUTTON,
            struct.pack("<BBI", device_mod.BUTTON_DOWN, device_mod.BUTTON_STATE_PRESSED, 1),
        )
        loop.run_once()  # must not raise

    def test_on_button_exception_does_not_crash_the_cycle(self):
        def boom(evt):
            raise RuntimeError("handler exploded")

        loop, _entries = _make_loop(self.device, on_button=boom)
        loop.run_once()
        self.serials["serial"].push_event(
            proto.Type.EVT_BUTTON,
            struct.pack("<BBI", device_mod.BUTTON_UP, device_mod.BUTTON_STATE_PRESSED, 1),
        )
        loop.run_once()  # must not raise despite the handler blowing up


class NotifyOverlayTests(unittest.TestCase):
    def setUp(self):
        self.device, self.serials, _clock = connected_device()
        self.addCleanup(self.device.close)
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.notify_path = os.path.join(self._tmp.name, "notify.json")
        self.preview_path = os.path.join(self._tmp.name, "preview.png")

    def test_active_request_is_composited_onto_the_frame(self):
        # notify_ipc's expiry is real wall-clock time (time.time(),
        # deliberately independent of the loop's own now_fn -- see
        # notify_ipc.py's module docstring), so this must use a real
        # "now" too rather than a fixed epoch like the loop's own
        # rendered-timestamp tests do.
        notify_ipc.write_request("task finished", seconds=60, path=self.notify_path)
        loop, _entries = _make_loop(
            self.device, notify_path=self.notify_path, preview_path=self.preview_path,
        )
        loop.run_once()

        img = Image.open(self.preview_path)
        # Top-left corner is inside the banner band -- must be dark.
        self.assertEqual(img.getpixel((1, 1)), 0)
        self.assertIsNotNone(loop._notify_active_req)

    def test_no_request_file_renders_normally(self):
        loop, _entries = _make_loop(
            self.device, notify_path=self.notify_path, preview_path=self.preview_path,
        )
        loop.run_once()
        self.assertIsNone(loop._notify_active_req)

    def test_expired_request_is_cleared_and_not_reapplied(self):
        # requested_at=0.0 (the unix epoch) is unconditionally in the past
        # relative to any real time.time() -- already-expired the moment
        # this loop first polls it.
        notify_ipc.write_request("expired-before-first-poll", seconds=0.001, path=self.notify_path, now=0.0)
        loop, _entries = _make_loop(
            self.device, notify_path=self.notify_path, preview_path=self.preview_path,
        )
        loop.run_once()
        self.assertFalse(os.path.exists(self.notify_path))
        self.assertIsNone(loop._notify_active_req)

    def test_disabled_when_notify_path_is_none(self):
        notify_ipc.write_request("x", seconds=60, path=self.notify_path)
        loop, _entries = _make_loop(
            self.device, notify_path=None, preview_path=self.preview_path,
        )
        loop.run_once()
        self.assertIsNone(loop._notify_active_req)


if __name__ == "__main__":
    unittest.main()
