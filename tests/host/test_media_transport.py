"""tests/host/test_media_transport.py -- byok.dashboard.media_transport."""

from __future__ import annotations

import os
import sys
import unittest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)

from byok import device as device_mod  # noqa: E402
from byok.dashboard import media_transport  # noqa: E402


def _evt(button, state=device_mod.BUTTON_STATE_PRESSED, t_ms=0):
    return device_mod.ButtonEvent(button, state, t_ms)


class ButtonHandlerTests(unittest.TestCase):
    def setUp(self):
        self.sent = []
        self.active = True
        self.handler = media_transport.make_button_handler(
            is_media_active=lambda: self.active, send=self.sent.append
        )

    def test_up_sends_next_track_when_media_active(self):
        self.handler(_evt(device_mod.BUTTON_UP))
        self.assertEqual(self.sent, ["next track"])

    def test_down_sends_previous_track(self):
        self.handler(_evt(device_mod.BUTTON_DOWN))
        self.assertEqual(self.sent, ["previous track"])

    def test_brightness_sends_playpause(self):
        self.handler(_evt(device_mod.BUTTON_BRIGHTNESS))
        self.assertEqual(self.sent, ["playpause"])

    def test_execute_and_wake_are_ignored(self):
        self.handler(_evt(device_mod.BUTTON_EXECUTE))
        self.handler(_evt(device_mod.BUTTON_WAKE))
        self.assertEqual(self.sent, [])

    def test_ignored_when_media_preset_not_active(self):
        self.active = False
        self.handler(_evt(device_mod.BUTTON_UP))
        self.assertEqual(self.sent, [])

    def test_only_a_clean_press_acts_not_release_repeat_or_long_press(self):
        for state in (
            device_mod.BUTTON_STATE_RELEASED,
            device_mod.BUTTON_STATE_AUTO_REPEAT,
            device_mod.BUTTON_STATE_LONG_PRESS,
        ):
            self.handler(_evt(device_mod.BUTTON_UP, state=state))
        self.assertEqual(self.sent, [])


class SendMediaCommandTests(unittest.TestCase):
    def test_calls_osascript_with_both_apps_in_one_script(self):
        calls = []

        import byok.dashboard.media_transport as mt

        real_run = mt.subprocess.run

        def fake_run(argv, **kwargs):
            calls.append(argv)

            class _Result:
                returncode = 0

            return _Result()

        mt.subprocess.run = fake_run
        try:
            mt.send_media_command("playpause")
        finally:
            mt.subprocess.run = real_run

        self.assertEqual(len(calls), 1)
        argv = calls[0]
        self.assertEqual(argv[0], "osascript")
        script = argv[2]
        self.assertIn("Music", script)
        self.assertIn("Spotify", script)
        self.assertIn("playpause", script)

    def test_never_raises_if_osascript_is_missing_or_fails(self):
        import byok.dashboard.media_transport as mt

        real_run = mt.subprocess.run

        def raising_run(*args, **kwargs):
            raise FileNotFoundError("no osascript")

        mt.subprocess.run = raising_run
        try:
            mt.send_media_command("next track")  # must not raise
        finally:
            mt.subprocess.run = real_run


if __name__ == "__main__":
    unittest.main()
