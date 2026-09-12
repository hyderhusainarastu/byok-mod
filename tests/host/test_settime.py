"""tests/host/test_settime.py -- `byok settime` / `Device.set_time()`
coverage: SET_TIME (0x27, docs/protocol.md §6.4 v1.1) payload encoding,
default local-time vs --utc behaviour, aware-datetime conversion, the
opaque `weekday` field, ACK handling, and NACK propagation.

Runnable:

    python3 -m unittest tests.host.test_settime -v

or, from the repo root:

    python3 -m unittest discover -s tests/host

Goes through a real `Device` + `SerialTransport` against a fake serial
port (`_fake_transport`, same technique as test_device_image.py /
test_dashboard_loop.py) -- proves `Device.set_time()` (what `cli.py
cmd_settime` calls) builds the exact 8-byte wire payload §6.4 documents
and firmware/s3/main/app_main.c's BYOK_TYPE_SET_TIME handler decodes
byte-for-byte, not just that the encoder helper does in isolation.
"""

import os
import struct
import sys
import unittest
from datetime import datetime, timedelta, timezone

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from byok import proto  # noqa: E402
from byok.device import NackReceived, _encode_set_time  # noqa: E402

from _fake_transport import RecordingSerial, connected_device, hello_ack_payload  # noqa: E402


# caps: default RecordingSerial bits (0,1,2,3,8 => 0x010F) plus bit 12
# (CAP_RTC, 0x1000) -- SET_TIME needs this bit set, per §6.6.
CAPS_WITH_RTC = 0x010F | proto.CAP_RTC


def _set_time_frames(serial):
    return [f for f in serial.written_frames if f.type == proto.Type.SET_TIME]


class EncodeSetTimeTests(unittest.TestCase):
    """§6.4: u16 year, u8 month/day/weekday/hour/minute/second, all
    little-endian, 8 bytes total -- exercised directly against a known
    datetime before going through the full Device/transport stack."""

    def test_known_datetime_payload_bytes(self):
        # 2026-09-03 14:30:07, weekday=3 (arbitrary opaque value here --
        # the field is never interpreted device-side, see §6.4).
        payload = _encode_set_time(2026, 9, 3, 3, 14, 30, 7)
        self.assertEqual(len(payload), 8)
        self.assertEqual(payload, struct.pack("<HBBBBBB", 2026, 9, 3, 3, 14, 30, 7))
        # Byte-by-byte per the documented offsets.
        self.assertEqual(payload[0:2], struct.pack("<H", 2026))  # year, little-endian
        self.assertEqual(payload[2], 9)   # month
        self.assertEqual(payload[3], 3)   # day
        self.assertEqual(payload[4], 3)   # weekday
        self.assertEqual(payload[5], 14)  # hour
        self.assertEqual(payload[6], 30)  # minute
        self.assertEqual(payload[7], 7)   # second

    def test_year_is_little_endian_u16(self):
        # 2199 = 0x0897 -> bytes 97 08 on the wire.
        payload = _encode_set_time(2199, 1, 1, 0, 0, 0, 0)
        self.assertEqual(payload[0:2], bytes([0x97, 0x08]))


class DeviceSetTimeTests(unittest.TestCase):
    """Goes through Device.set_time() -> a real SET_TIME frame on the
    wire, against a fake serial port that ACKs by default (RecordingSerial,
    the same fake test_device_image.py uses)."""

    def _device(self):
        payload = hello_ack_payload(caps=CAPS_WITH_RTC)
        return connected_device(hello_payload=payload)

    def test_explicit_naive_datetime_sends_exact_fields(self):
        device, serials, _clock = self._device()
        dt = datetime(2026, 9, 3, 14, 30, 7)
        sent = device.set_time(dt, weekday=3)
        self.assertEqual(sent, dt)

        frames = _set_time_frames(serials["serial"])
        self.assertEqual(len(frames), 1)
        frame = frames[0]
        self.assertTrue(frame.flags & proto.Flags.ACK_REQ)
        self.assertEqual(frame.payload, _encode_set_time(2026, 9, 3, 3, 14, 30, 7))

    def test_default_weekday_matches_pythons_own_convention(self):
        # 2026-09-03 is a Thursday -> datetime.weekday() == 3 (Monday=0).
        # §6.4: the field is opaque on the wire, but Device.set_time()
        # documents that it uses Python's own weekday() as its default --
        # this pins that choice.
        device, serials, _clock = self._device()
        dt = datetime(2026, 9, 3, 8, 0, 0)
        self.assertEqual(dt.weekday(), 3)
        device.set_time(dt)

        frame = _set_time_frames(serials["serial"])[0]
        self.assertEqual(frame.payload[4], 3)

    def test_no_args_sends_current_local_time(self):
        device, serials, _clock = self._device()
        before = datetime.now()
        sent = device.set_time()
        after = datetime.now()

        self.assertLessEqual(before, sent)
        self.assertLessEqual(sent, after)

        frame = _set_time_frames(serials["serial"])[0]
        year, month, day, weekday, hour, minute, second = struct.unpack("<HBBBBBB", frame.payload)
        self.assertEqual((year, month, day), (sent.year, sent.month, sent.day))
        self.assertEqual((hour, minute, second), (sent.hour, sent.minute, sent.second))
        self.assertEqual(weekday, sent.weekday())

    def test_utc_true_with_no_dt_sends_current_utc_time(self):
        device, serials, _clock = self._device()
        before = datetime.now(timezone.utc)
        sent = device.set_time(utc=True)
        after = datetime.now(timezone.utc)

        # set_time(utc=True) returns the aware UTC datetime it built and
        # sent (tzinfo intact) -- compare aware-to-aware.
        self.assertLessEqual(before, sent)
        self.assertLessEqual(sent, after)

        frame = _set_time_frames(serials["serial"])[0]
        year, month, day, weekday, hour, minute, second = struct.unpack("<HBBBBBB", frame.payload)
        self.assertEqual((year, month, day, hour, minute, second),
                          (sent.year, sent.month, sent.day, sent.hour, sent.minute, sent.second))

    def test_aware_datetime_converted_to_utc_when_utc_true(self):
        device, serials, _clock = self._device()
        # A fixed +05:30 offset (arbitrary, deterministic -- not the host's
        # own local zone, so this proves conversion actually happens).
        tz = timezone(timedelta(hours=5, minutes=30))
        dt = datetime(2026, 9, 3, 20, 0, 0, tzinfo=tz)
        sent = device.set_time(dt, utc=True)

        expected = dt.astimezone(timezone.utc)
        self.assertEqual((sent.year, sent.month, sent.day, sent.hour, sent.minute, sent.second),
                          (expected.year, expected.month, expected.day,
                           expected.hour, expected.minute, expected.second))
        # 20:00 +05:30 -> 14:30 UTC, and the date does not roll over.
        self.assertEqual((sent.month, sent.day, sent.hour, sent.minute), (9, 3, 14, 30))

        frame = _set_time_frames(serials["serial"])[0]
        year, month, day, weekday, hour, minute, second = struct.unpack("<HBBBBBB", frame.payload)
        self.assertEqual((month, day, hour, minute), (9, 3, 14, 30))

    def test_aware_datetime_converted_to_local_when_utc_false(self):
        device, serials, _clock = self._device()
        tz = timezone(timedelta(hours=5, minutes=30))
        dt = datetime(2026, 9, 3, 20, 0, 0, tzinfo=tz)
        sent = device.set_time(dt, utc=False)

        expected = dt.astimezone()  # host's local zone
        self.assertEqual((sent.year, sent.month, sent.day, sent.hour, sent.minute, sent.second),
                          (expected.year, expected.month, expected.day,
                           expected.hour, expected.minute, expected.second))

    def test_ack_does_not_raise(self):
        # RecordingSerial ACKs every ACK_REQ'd frame by default -- proves
        # the request/reply plumbing (SET_TIME -> ACK, no NACK path) works
        # end to end with no exception.
        device, _serials, _clock = self._device()
        try:
            device.set_time(datetime(2026, 1, 1, 0, 0, 0))
        except NackReceived:
            self.fail("set_time() raised NackReceived on a plain ACK reply")


class DeviceSetTimeNackTests(unittest.TestCase):
    """NACK handling: a device with no working RTC (or one refusing a
    field) answers E_UNSUPPORTED / E_BAD_PARAM instead of ACK, per §6.4 --
    Device.set_time() must surface that as NackReceived, not swallow it or
    misreport ACK."""

    class _NackingSerial(RecordingSerial):
        """Like RecordingSerial, but answers SET_TIME with a scripted
        NACK instead of the generic ACK every other ACK_REQ'd type gets."""

        def __init__(self, *, code, detail=0, **kwargs):
            super().__init__(**kwargs)
            self._nack_code = code
            self._nack_detail = detail

        def _on_frame(self, frame):
            if frame.type != proto.Type.SET_TIME:
                # HELLO (and anything else) still gets RecordingSerial's
                # normal generic reply, including recording into
                # written_frames.
                return super()._on_frame(frame)

            self.written_frames.append(frame)
            payload = struct.pack("<BBH", self._nack_code, self._nack_detail, frame.seq)
            reply = proto.encode_frame(proto.Type.NACK, proto.Flags.IS_REPLY, frame.seq, payload)
            self._rx.extend(reply)

    def _device_with_nack(self, code, detail=0):
        payload = hello_ack_payload(caps=CAPS_WITH_RTC)

        def factory(_device_path):
            return self._NackingSerial(code=code, detail=detail, hello_payload=payload)

        return connected_device(hello_payload=payload, serial_factory=factory)

    def test_e_unsupported_raises_nack_received(self):
        device, _serials, _clock = self._device_with_nack(proto.ErrorCode.E_UNSUPPORTED)
        with self.assertRaises(NackReceived) as ctx:
            device.set_time(datetime(2026, 1, 1, 0, 0, 0))
        self.assertEqual(ctx.exception.code, proto.ErrorCode.E_UNSUPPORTED)

    def test_e_bad_param_raises_nack_received_with_field_offset(self):
        # §6.4: NACK/E_BAD_PARAM.detail is the offending field's byte
        # offset -- offset 5 is `hour`.
        device, _serials, _clock = self._device_with_nack(proto.ErrorCode.E_BAD_PARAM, detail=5)
        with self.assertRaises(NackReceived) as ctx:
            device.set_time(datetime(2026, 1, 1, 0, 0, 0))
        self.assertEqual(ctx.exception.code, proto.ErrorCode.E_BAD_PARAM)
        self.assertEqual(ctx.exception.detail, 5)


class CliSettimeParserTests(unittest.TestCase):
    """Pure argparse-level coverage for `byok settime` -- no device, no
    serial port; just proves the subcommand is wired up with the expected
    flags/defaults (build_parser() -> cmd_settime)."""

    def setUp(self):
        from byok import cli
        self.cli = cli

    def test_settime_defaults(self):
        args = self.cli.build_parser().parse_args(["settime"])
        self.assertEqual(args.command, "settime")
        self.assertIsNone(args.datetime)
        self.assertFalse(args.utc)
        self.assertIs(args.func, self.cli.cmd_settime)

    def test_settime_with_datetime_and_utc(self):
        args = self.cli.build_parser().parse_args(["settime", "--utc", "2026-09-03T14:30:00"])
        self.assertEqual(args.datetime, "2026-09-03T14:30:00")
        self.assertTrue(args.utc)


if __name__ == "__main__":
    unittest.main()
