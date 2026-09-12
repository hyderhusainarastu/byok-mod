"""tests/host/test_notify.py -- byok.notify_ipc (the byok notify state
file + composite-overlay drawing helper)."""

from __future__ import annotations

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

from byok import cli, notify_ipc  # noqa: E402


class RequestFileTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.path = os.path.join(self._tmp.name, "sub", "notify.json")

    def test_write_then_read_round_trips(self):
        written = notify_ipc.write_request("hello", seconds=10, path=self.path, now=1000.0)
        read = notify_ipc.read_request(self.path)
        self.assertEqual(read, written)
        self.assertEqual(read.text, "hello")
        self.assertEqual(read.requested_at, 1000.0)
        self.assertEqual(read.expires_at, 1010.0)

    def test_read_missing_file_returns_none(self):
        self.assertIsNone(notify_ipc.read_request(self.path))

    def test_read_corrupt_file_returns_none_not_raise(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as fh:
            fh.write("{not valid json")
        self.assertIsNone(notify_ipc.read_request(self.path))

    def test_clear_removes_the_file(self):
        notify_ipc.write_request("hi", path=self.path)
        self.assertTrue(os.path.exists(self.path))
        notify_ipc.clear_request(self.path)
        self.assertFalse(os.path.exists(self.path))

    def test_clear_missing_file_does_not_raise(self):
        notify_ipc.clear_request(self.path)  # no file exists yet

    def test_second_write_replaces_the_first(self):
        notify_ipc.write_request("first", path=self.path, now=0.0)
        notify_ipc.write_request("second", path=self.path, now=100.0)
        read = notify_ipc.read_request(self.path)
        self.assertEqual(read.text, "second")


class IsActiveTests(unittest.TestCase):
    def test_none_is_never_active(self):
        self.assertFalse(notify_ipc.is_active(None, now=0.0))

    def test_active_within_window(self):
        req = notify_ipc.NotifyRequest(text="x", requested_at=100.0, expires_at=110.0)
        self.assertTrue(notify_ipc.is_active(req, now=105.0))

    def test_inactive_before_requested_at(self):
        req = notify_ipc.NotifyRequest(text="x", requested_at=100.0, expires_at=110.0)
        self.assertFalse(notify_ipc.is_active(req, now=99.0))

    def test_inactive_at_or_after_expiry(self):
        req = notify_ipc.NotifyRequest(text="x", requested_at=100.0, expires_at=110.0)
        self.assertFalse(notify_ipc.is_active(req, now=110.0))
        self.assertFalse(notify_ipc.is_active(req, now=200.0))


class LoopLockTests(unittest.TestCase):
    """byok.notify_ipc's loop lock (write_lock/read_lock/clear_lock/
    loop_is_running) -- lets `cli.py`'s `cmd_notify` tell a running
    dashboard/mirror loop from a dead one without ever opening the port
    (incident 2026-09-04; see notify_ipc.write_lock's own docstring)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.path = os.path.join(self._tmp.name, "sub", "loop.lock")

    def test_write_then_read_round_trips(self):
        written = notify_ipc.write_lock(self.path, pid=4242)
        read = notify_ipc.read_lock(self.path)
        self.assertEqual(read, written)
        self.assertEqual(read.pid, 4242)

    def test_write_defaults_to_this_process_pid(self):
        written = notify_ipc.write_lock(self.path)
        self.assertEqual(written.pid, os.getpid())

    def test_read_missing_file_returns_none(self):
        self.assertIsNone(notify_ipc.read_lock(self.path))

    def test_read_corrupt_file_returns_none_not_raise(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as fh:
            fh.write("{not valid json")
        self.assertIsNone(notify_ipc.read_lock(self.path))

    def test_clear_removes_the_file(self):
        notify_ipc.write_lock(self.path, pid=os.getpid())
        self.assertTrue(os.path.exists(self.path))
        notify_ipc.clear_lock(self.path)
        self.assertFalse(os.path.exists(self.path))

    def test_clear_missing_file_does_not_raise(self):
        notify_ipc.clear_lock(self.path)  # no file exists yet

    def test_loop_is_running_false_when_no_lock_file(self):
        self.assertFalse(notify_ipc.loop_is_running(self.path))

    def test_loop_is_running_true_for_a_live_pid(self):
        notify_ipc.write_lock(self.path, pid=os.getpid())
        self.assertTrue(notify_ipc.loop_is_running(self.path))

    def test_loop_is_running_false_for_a_stale_lock(self):
        # A lock left behind by a process that died without cleanup
        # (SIGKILL) must not permanently strand cmd_notify in IPC-only
        # mode -- os.kill(pid, 0) raising ProcessLookupError is exactly
        # what a dead pid looks like.
        notify_ipc.write_lock(self.path, pid=999999)
        with mock.patch("byok.notify_ipc.os.kill", side_effect=ProcessLookupError):
            self.assertFalse(notify_ipc.loop_is_running(self.path))

    def test_pid_alive_true_for_our_own_pid(self):
        self.assertTrue(notify_ipc._pid_alive(os.getpid()))

    def test_pid_alive_false_for_nonpositive_pid(self):
        self.assertFalse(notify_ipc._pid_alive(0))
        self.assertFalse(notify_ipc._pid_alive(-1))


class CmdNotifyIpcFirstTests(unittest.TestCase):
    """cli.py cmd_notify's IPC-first behavior (host fix, incident
    2026-09-04): never opens the device when a loop's lock says one is
    already running, and falls back to the IPC request file -- instead of
    letting the exception propagate -- when Device.open() raises
    PortBusy. Device.open() and notify_ipc are mocked; no real device, no
    real filesystem writes."""

    def setUp(self):
        self.args = cli.build_parser().parse_args(["notify", "hello", "--seconds", "3"])

    def test_loop_running_skips_device_open_entirely(self):
        with mock.patch.object(cli.notify_ipc, "loop_is_running", return_value=True), \
             mock.patch.object(cli.notify_ipc, "write_request") as write_request, \
             mock.patch.object(cli.Device, "open") as device_open:
            rc = cli.cmd_notify(self.args)
        self.assertEqual(rc, 0)
        device_open.assert_not_called()
        write_request.assert_called_once_with("hello", seconds=3.0)

    def test_port_busy_falls_back_to_ipc_without_raising(self):
        with mock.patch.object(cli.notify_ipc, "loop_is_running", return_value=False), \
             mock.patch.object(cli.notify_ipc, "write_request") as write_request, \
             mock.patch.object(
                 cli.Device, "open", side_effect=cli.PortBusy("port busy (simulated)")
             ):
            rc = cli.cmd_notify(self.args)
        self.assertEqual(rc, 0)
        write_request.assert_called_once_with("hello", seconds=3.0)

    def test_no_loop_and_device_reachable_draws_directly_not_ipc(self):
        fake_dev = mock.MagicMock()
        fake_dev.display_size = (240, 80)
        with mock.patch.object(cli.notify_ipc, "loop_is_running", return_value=False), \
             mock.patch.object(cli.notify_ipc, "write_request") as write_request, \
             mock.patch.object(cli.Device, "open", return_value=fake_dev), \
             mock.patch.object(cli.time, "sleep"):
            rc = cli.cmd_notify(self.args)
        self.assertEqual(rc, 0)
        write_request.assert_not_called()
        fake_dev.close.assert_called_once()


class OverlayBannerTests(unittest.TestCase):
    def test_draws_a_dark_band_across_the_top(self):
        img = Image.new("L", (240, 80), color=255)
        notify_ipc.overlay_banner(img, "task finished")
        # Top-left corner (inside the filled band) must have gone dark;
        # the very bottom row (well outside any plausible banner height,
        # even the image.height//3 clamp) must be untouched.
        self.assertEqual(img.getpixel((1, 1)), 0)
        self.assertEqual(img.getpixel((1, 79)), 255)

    def test_never_exceeds_a_third_of_the_image_height(self):
        img = Image.new("L", (240, 30), color=255)
        notify_ipc.overlay_banner(img, "x", height=100)  # requests more than allowed
        self.assertEqual(img.getpixel((1, 29)), 255)  # bottom row still untouched

    def test_does_not_raise_on_empty_text(self):
        img = Image.new("L", (240, 80), color=255)
        notify_ipc.overlay_banner(img, "")

    def test_does_not_raise_on_very_long_text(self):
        img = Image.new("L", (240, 80), color=255)
        notify_ipc.overlay_banner(img, "a very long notification message that will not fit on one small banner row")


if __name__ == "__main__":
    unittest.main()
