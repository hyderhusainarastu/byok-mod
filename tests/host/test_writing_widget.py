"""tests/host/test_writing_widget.py -- byok.dashboard.widgets.writing +
the device.py GET_DOCSTATS/DOCSTATS wire helpers it's built on.

Uses `_fake_transport.connected_device()` (a real `Device` +
`SerialTransport` over a fake serial port, same technique as
test_device_image.py) with a scripted DOCSTATS reply, so
`DeviceDocStatsProvider` is exercised against the real encode/decode path
end to end, not a hand-rolled stand-in.
"""

from __future__ import annotations

import datetime
import os
import struct
import sys
import unittest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from byok import proto  # noqa: E402
from byok.dashboard.fonts import FontSet  # noqa: E402
from byok.dashboard.widgets import writing as w  # noqa: E402
from byok.dashboard.widgets.base import RenderContext  # noqa: E402
from byok.device import NackReceived  # noqa: E402

from _fake_transport import connected_device  # noqa: E402

_FONTS = FontSet()


def _docstats_payload(files=12, words=48213, doc_bytes=290000, newest_epoch=1788400000, words_today=612) -> bytes:
    return struct.pack("<IIIII", files, words, doc_bytes, newest_epoch, words_today)


class DeviceGetDocstatsTests(unittest.TestCase):
    """`Device.get_docstats()` itself (proto.py Type.GET_DOCSTATS / .DOCSTATS)."""

    def test_decodes_a_scripted_docstats_reply(self):
        device, _serials, _clock = connected_device(
            type_replies={proto.Type.GET_DOCSTATS: (proto.Type.DOCSTATS, _docstats_payload())}
        )
        try:
            stats = device.get_docstats()
        finally:
            device.close()
        self.assertEqual(stats.files, 12)
        self.assertEqual(stats.words, 48213)
        self.assertEqual(stats.bytes, 290000)
        self.assertEqual(stats.newest_epoch, 1788400000)
        self.assertEqual(stats.words_today, 612)

    def test_nack_from_older_firmware_raises_nack_received(self):
        # A firmware build that predates GET_DOCSTATS answers with
        # NACK/E_UNKNOWN_TYPE, per docs/protocol.md §5's "unknown type"
        # rule -- this is the exact failure DeviceDocStatsProvider must
        # degrade from rather than propagate.
        def nack_reply(frame):
            payload = struct.pack("<BBH", proto.ErrorCode.E_UNKNOWN_TYPE, proto.Type.GET_DOCSTATS, frame.seq)
            return proto.Type.NACK, payload

        device, _serials, _clock = connected_device(
            type_replies={proto.Type.GET_DOCSTATS: nack_reply}
        )
        try:
            with self.assertRaises(NackReceived):
                device.get_docstats()
        finally:
            device.close()

    def test_set_presets_sends_fixed_width_nul_padded_names(self):
        # docs/protocol.md §6.4b: `u8 count` then `count` fixed 20-byte
        # NUL-padded name fields -- LEN must equal exactly 1 + count*20.
        device, serials, _clock = connected_device()
        try:
            device.set_presets(["Clock", "Sample Preset Name!!", "A" * 30])
        finally:
            device.close()
        sent = [f for f in serials["serial"].written_frames if f.type == proto.Type.SET_PRESETS]
        self.assertEqual(len(sent), 1)
        payload = sent[0].payload
        self.assertEqual(len(payload), 1 + 3 * 20)
        count = payload[0]
        self.assertEqual(count, 3)
        self.assertEqual(payload[1:21], b"Clock" + b"\x00" * 15)
        self.assertEqual(payload[21:41], b"Sample Preset Name!!")
        # Third entry (30 'A's) is truncated to exactly 20 bytes -- no
        # room for a NUL pad, and the payload doesn't grow past it.
        self.assertEqual(payload[41:61], b"A" * 20)

    def test_set_presets_rejects_more_than_eight(self):
        device, _serials, _clock = connected_device()
        try:
            with self.assertRaises(ValueError):
                device.set_presets([f"P{i}" for i in range(9)])
        finally:
            device.close()

    def test_display_cfg_bulk_on_and_off(self):
        # docs/protocol.md §6.4b: `u8 flags`, bit0 = bulk I2C writes.
        device, serials, _clock = connected_device()
        try:
            device.display_cfg_bulk(True)
            device.display_cfg_bulk(False)
        finally:
            device.close()
        sent = [f for f in serials["serial"].written_frames if f.type == proto.Type.DISPLAY_CFG]
        self.assertEqual(len(sent), 2)
        self.assertEqual(sent[0].payload, bytes([0x01]))  # bulk on
        self.assertEqual(sent[1].payload, bytes([0x00]))  # bulk off


class DeviceDocStatsProviderTests(unittest.TestCase):
    def test_wraps_a_duck_typed_device(self):
        class FakeDevice:
            def __init__(self):
                self.calls = 0

            def get_docstats(self):
                self.calls += 1
                from byok.device import DocStats

                return DocStats(files=3, words=1000, doc_bytes=6000, newest_epoch=1700000000, words_today=50)

        fake = FakeDevice()
        clock = {"t": 0.0}
        provider = w.DeviceDocStatsProvider(fake, min_interval_s=10.0, clock=lambda: clock["t"])

        s1 = provider.stats()
        self.assertEqual(s1.words, 1000)
        self.assertEqual(fake.calls, 1)

        # Within the throttle window: cached, no new call.
        clock["t"] = 5.0
        s2 = provider.stats()
        self.assertEqual(s2.words, 1000)
        self.assertEqual(fake.calls, 1)

        # Past the throttle window: refetches.
        clock["t"] = 11.0
        provider.stats()
        self.assertEqual(fake.calls, 2)

    def test_device_exception_degrades_to_error_field_not_raise(self):
        class Boom:
            def get_docstats(self):
                raise RuntimeError("link dead")

        provider = w.DeviceDocStatsProvider(Boom(), clock=lambda: 0.0)
        s = provider.stats()
        self.assertIsNone(s.words)
        self.assertIsNotNone(s.error)


class NullProviderTests(unittest.TestCase):
    def test_all_none_no_io(self):
        s = w.NullProvider().stats()
        self.assertIsNone(s.words)
        self.assertIsNone(s.files)
        self.assertIsNone(s.words_today)


class WidgetRenderTests(unittest.TestCase):
    _NOW = datetime.datetime(2026, 9, 4, 15, 0, 0)

    def _ctx(self, width=240, height=32, options=None, providers=None):
        return RenderContext(width=width, height=height, now=self._NOW, options=options or {},
                              fonts=_FONTS, providers=providers or {})

    def test_null_provider_renders_dash_without_raising(self):
        img = w.WritingWidget().render(self._ctx())
        self.assertEqual(img.size, (240, 32))

    def test_populated_stats_render_at_several_heights_with_goal_bar(self):
        import time

        class Fixed(w.WritingProvider):
            def stats(self):
                return w.WritingStats(
                    files=12, words=48213, bytes=290000,
                    newest_epoch=time.mktime(WidgetRenderTests._NOW.timetuple()) - 600,
                    words_today=612,
                )

        for height in (16, 24, 32, 48):
            with self.subTest(height=height):
                ctx = self._ctx(height=height, options={"daily_goal_words": 500},
                                 providers={"writing": Fixed()})
                img = w.WritingWidget().render(ctx)
                self.assertEqual(img.size, (240, height))

    def test_provider_exception_does_not_crash_render(self):
        class Boom(w.WritingProvider):
            def stats(self):
                raise RuntimeError("boom")

        img = w.WritingWidget().render(self._ctx(providers={"writing": Boom()}))
        self.assertEqual(img.size, (240, 32))

    def test_zero_or_missing_goal_does_not_divide_by_zero(self):
        class Fixed(w.WritingProvider):
            def stats(self):
                return w.WritingStats(files=1, words=100, words_today=10, newest_epoch=None)

        ctx = self._ctx(height=48, options={"daily_goal_words": 0}, providers={"writing": Fixed()})
        img = w.WritingWidget().render(ctx)  # must not raise ZeroDivisionError
        self.assertEqual(img.size, (240, 48))


if __name__ == "__main__":
    unittest.main()
