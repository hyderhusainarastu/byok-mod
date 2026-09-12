"""Tests for byok.transport: port discovery, HELLO handshake, reconnect.

No real serial port is ever touched here — every test uses a fake serial
object and, where discovery is exercised, a fake `comports()` list. See
byok/transport.py's module docstring for why matching only succeeds on the
firmware's product string.
"""

import os
import struct
import sys
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "host", "macos"))

from byok import proto  # noqa: E402
from byok import transport  # noqa: E402


# --------------------------------------------------------------------------
# Fakes
# --------------------------------------------------------------------------


class ManualClock:
    """A fake monotonic clock/sleep pair with no real wall-time cost.

    `sleep(s)` just advances the clock instead of blocking, so timeout and
    backoff logic can be exercised instantly and deterministically.
    """

    def __init__(self):
        self.t = 0.0
        self.sleep_calls = []

    def time(self) -> float:
        return self.t

    def sleep(self, seconds: float) -> None:
        self.sleep_calls.append(seconds)
        self.t += seconds


class ScriptedSerial:
    """A fake serial port that decodes frames as they're written (using the
    real byok.proto.Parser — the same decoder the firmware side must agree
    with) and can synchronously enqueue a scripted response, modelling a
    device that replies instantly.

    `responder(frame) -> Optional[bytes]` receives each decoded outbound
    Frame and returns raw bytes to enqueue for the next read() (or None to
    simulate silence)."""

    def __init__(self, responder=None):
        self.is_open = True
        self._rx = bytearray()
        self.written_frames = []
        self._responder = responder
        self._parser = proto.Parser(on_frame=self._on_frame)

    def _on_frame(self, frame: proto.Frame) -> None:
        self.written_frames.append(frame)
        if self._responder is not None:
            reply_bytes = self._responder(frame)
            if reply_bytes:
                self._rx.extend(reply_bytes)

    def write(self, data: bytes) -> int:
        self._parser.feed(data)
        return len(data)

    def read(self, n: int = 1) -> bytes:
        chunk = bytes(self._rx[:n])
        del self._rx[: len(chunk)]
        return chunk

    def close(self) -> None:
        self.is_open = False


def _fake_hello_ack_payload(device_id: bytes = b"\x01" * 8) -> bytes:
    return struct.pack(
        "<BBBBHHBBHIBBHI",
        1,  # proto_ver
        1, 2, 3,  # fw_major, fw_minor, fw_patch
        122, 250,  # display_w, display_h
        1, 2,  # bpp_native, bpp_max
        4096,  # dev_max_payload
        0x010F,  # caps
        2,  # boot_slot (ota_1)
        1,  # is_our_firmware
        3,  # fonts
        12345,  # uptime_ms
    ) + device_id


def hello_ack_responder(frame: proto.Frame):
    """A responder function: answers HELLO with a valid HELLO_ACK, ignores
    everything else."""
    if frame.type == proto.Type.HELLO:
        payload = _fake_hello_ack_payload()
        return proto.encode_frame(proto.Type.HELLO_ACK, proto.Flags.IS_REPLY, frame.seq, payload)
    return None


def silent_responder(frame: proto.Frame):
    return None


def make_port_info(device: str, product=None, description=None):
    ns = types.SimpleNamespace()
    ns.device = device
    ns.product = product
    ns.description = description
    ns.vid = None
    ns.pid = None
    ns.serial_number = None
    return ns


# transport.SerialTransport._resolve_device() requires an explicit `device`
# to also appear in list_candidates() with a matching product string (see
# transport.py's module docstring and WrongDevice) -- so every fixed-device
# test below must inject a comports() fixture that actually matches
# "/dev/cu.usbmodemFAKE", or connect() now raises WrongDevice instead of
# reaching the fake serial_factory.
FAKE_MATCHING_COMPORTS = [
    make_port_info("/dev/cu.usbmodemFAKE", product="BYOK Mod Display", description="BYOK Mod Display")
]


# --------------------------------------------------------------------------
# Discovery
# --------------------------------------------------------------------------


class DiscoveryTests(unittest.TestCase):
    def test_matches_only_our_product_string(self):
        ports = [
            make_port_info("/dev/cu.usbmodem1101", product="BYOK Mod Display", description="BYOK Mod Display"),
            make_port_info("/dev/cu.usbmodem2201", product="Some Other Device"),
            make_port_info("/dev/cu.Bluetooth-Incoming-Port", product=None, description=None),
        ]
        found = transport.find_device_port(ports)
        self.assertIsNotNone(found)
        self.assertEqual(found.device, "/dev/cu.usbmodem1101")

    def test_no_match_returns_none(self):
        ports = [
            make_port_info("/dev/cu.usbmodem2201", product="Some Other Device"),
            make_port_info("/dev/cu.usbmodem3301", description="USB JTAG/serial debug unit"),
        ]
        self.assertIsNone(transport.find_device_port(ports))

    def test_non_usbmodem_devices_are_never_candidates(self):
        # Guards the "never touch /dev/cu.* on pattern alone" rule: even a
        # device node with a matching product string is excluded if it
        # isn't a usbmodem/usbserial node.
        ports = [make_port_info("/dev/cu.Bluetooth-Incoming-Port", product="BYOK Mod Display")]
        self.assertEqual(transport.list_candidates(ports), [])
        self.assertIsNone(transport.find_device_port(ports))

    def test_empty_port_list_returns_none(self):
        self.assertIsNone(transport.find_device_port([]))

    def test_connect_never_opens_serial_when_nothing_matches(self):
        def factory_should_not_be_called(device):
            raise AssertionError(f"serial_factory must not be called, got device={device!r}")

        t = transport.SerialTransport(serial_factory=factory_should_not_be_called, comports=[])
        with self.assertRaises(transport.NoDeviceFound):
            t.connect()

    def test_explicit_port_that_does_not_match_is_refused(self):
        """Regression: an explicit --port used to be opened verbatim with
        NO product-string check at all, directly contradicting this
        module's own docstring invariant. A --port whose product string
        doesn't match ours must now be refused (WrongDevice), not opened
        -- covering e.g. the stock firmware's port, or an unrelated USB
        device."""

        def factory_should_not_be_called(device):
            raise AssertionError(f"serial_factory must not be called, got device={device!r}")

        ports = [make_port_info("/dev/cu.usbmodem9999", product="Some Other Device")]
        t = transport.SerialTransport(
            device="/dev/cu.usbmodem9999",
            serial_factory=factory_should_not_be_called,
            comports=ports,
        )
        with self.assertRaises(transport.WrongDevice):
            t.connect()

    def test_explicit_port_not_present_at_all_is_refused(self):
        # The node isn't even in the candidate list (e.g. it's a
        # non-usbmodem node, or nothing is plugged in there) -- still
        # refused, not opened.
        def factory_should_not_be_called(device):
            raise AssertionError(f"serial_factory must not be called, got device={device!r}")

        t = transport.SerialTransport(
            device="/dev/cu.usbmodem9999",
            serial_factory=factory_should_not_be_called,
            comports=[],
        )
        with self.assertRaises(transport.WrongDevice):
            t.connect()

    def test_allow_unmatched_port_bypasses_the_check(self):
        # The explicit, separately-named escape hatch (cli.py's
        # --unsafe-port) still works, and only it does.
        ports = [make_port_info("/dev/cu.usbmodem9999", product="Some Other Device")]
        serial = ScriptedSerial(responder=hello_ack_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodem9999",
            serial_factory=lambda device: serial,
            comports=ports,
            allow_unmatched_port=True,
        )
        t.connect(do_hello=True)  # does not raise
        self.assertIsNotNone(t.hello_ack)

    def test_default_serial_factory_never_asserts_dtr_or_rts(self):
        """Regression: pyserial asserts DTR/RTS on open by default
        (dsrdtr/rtscts flow control) -- the exact line-state combination
        the ESP32-S3's USB-Serial-JTAG interprets as reset-to-bootloader.
        The default factory must open with both disabled and drive both
        lines low explicitly, never leaving them at pyserial's default."""
        fake_serial_instance = mock.MagicMock()
        fake_serial_cls = mock.MagicMock(return_value=fake_serial_instance)
        fake_serial_module = types.SimpleNamespace(Serial=fake_serial_cls)

        with mock.patch.dict(sys.modules, {"serial": fake_serial_module}):
            result = transport.SerialTransport._default_serial_factory("/dev/cu.usbmodemFAKE")

        self.assertIs(result, fake_serial_instance)
        _args, kwargs = fake_serial_cls.call_args
        self.assertEqual(kwargs.get("dsrdtr"), False)
        self.assertEqual(kwargs.get("rtscts"), False)
        self.assertEqual(fake_serial_instance.dtr, False)
        self.assertEqual(fake_serial_instance.rts, False)

    def test_default_serial_factory_opens_exclusive(self):
        """Regression, incident 2026-09-04 (see transport.PortBusy's
        docstring): a second process opening the same port used to
        succeed silently and could wipe a running loop's SEQ session.
        The default factory must open with exclusive=True so a second
        open fails loudly instead."""
        fake_serial_instance = mock.MagicMock()
        fake_serial_cls = mock.MagicMock(return_value=fake_serial_instance)
        fake_serial_module = types.SimpleNamespace(Serial=fake_serial_cls)

        with mock.patch.dict(sys.modules, {"serial": fake_serial_module}):
            transport.SerialTransport._default_serial_factory("/dev/cu.usbmodemFAKE")

        _args, kwargs = fake_serial_cls.call_args
        self.assertEqual(kwargs.get("exclusive"), True)


class PortBusyTests(unittest.TestCase):
    """connect() distinguishing "another process holds this port"
    (PortBusy) from every other open failure -- see PortBusy's own
    docstring for the incident this fixes."""

    def test_exclusive_lock_failure_raises_port_busy(self):
        def busy_factory(device):
            # The exact wording pyserial's posix backend raises (see
            # transport._is_port_busy_exception's docstring) -- a plain
            # OSError here, not a real serial.SerialException, proves
            # the check is on message text, not isinstance.
            raise OSError(
                35, f"Could not exclusively lock port {device}: "
                "[Errno 35] Resource temporarily unavailable"
            )

        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=busy_factory,
        )
        with self.assertRaises(transport.PortBusy):
            t.connect(do_hello=False)

    def test_port_busy_is_a_transport_error(self):
        self.assertTrue(issubclass(transport.PortBusy, transport.TransportError))

    def test_unrelated_oserror_is_not_reported_as_port_busy(self):
        def other_factory(device):
            raise OSError(2, "No such file or directory")

        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=other_factory,
        )
        with self.assertRaises(OSError) as ctx:
            t.connect(do_hello=False)
        self.assertNotIsInstance(ctx.exception, transport.PortBusy)


# --------------------------------------------------------------------------
# HELLO handshake
# --------------------------------------------------------------------------


class ForceMockTests(unittest.TestCase):
    """Regression for the 2026-09-04 incident: `byok notify` was run via a
    bare Bash invocation (not through the mocked test suite) and opened a
    real serial port twice, with no way to have prevented it. BYOK_FORCE_MOCK
    is the safety net -- an automated/dev shell can export it to make any
    connect() that would open a real port raise instead."""

    def setUp(self):
        self._had = "BYOK_FORCE_MOCK" in os.environ
        self._prior = os.environ.get("BYOK_FORCE_MOCK")

    def tearDown(self):
        if self._had:
            os.environ["BYOK_FORCE_MOCK"] = self._prior
        else:
            os.environ.pop("BYOK_FORCE_MOCK", None)

    def test_force_mock_refuses_default_factory_even_on_a_match(self):
        # A real, matching device is present -- connect() would normally
        # open it. With BYOK_FORCE_MOCK set and no serial_factory override,
        # it must refuse before ever touching the OS-level serial API.
        ports = [make_port_info("/dev/cu.usbmodem1101", product="BYOK Mod Display")]
        os.environ["BYOK_FORCE_MOCK"] = "1"
        t = transport.SerialTransport(comports=ports)
        with self.assertRaises(transport.MockRequired):
            t.connect()

    def test_force_mock_allows_an_explicit_serial_factory_override(self):
        ports = [make_port_info("/dev/cu.usbmodem1101", product="BYOK Mod Display")]
        os.environ["BYOK_FORCE_MOCK"] = "1"
        ser = ScriptedSerial([])
        t = transport.SerialTransport(serial_factory=lambda device: ser, comports=ports)
        t.connect(do_hello=False)  # must NOT raise: an explicit fake factory was supplied
        self.assertTrue(t.is_connected())

    def test_unset_env_var_behaves_exactly_as_before(self):
        os.environ.pop("BYOK_FORCE_MOCK", None)
        t = transport.SerialTransport(serial_factory=None, comports=[])
        with self.assertRaises(transport.NoDeviceFound):
            t.connect()  # unrelated failure (no match) -- proves the guard didn't fire


class HelloHandshakeTests(unittest.TestCase):
    def test_hello_success_populates_hello_ack(self):
        clock = ManualClock()
        serial = ScriptedSerial(responder=hello_ack_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=lambda device: serial,
            clock=clock.time,
            sleep=clock.sleep,
        )
        t.connect(do_hello=True)

        self.assertIsNotNone(t.hello_ack)
        self.assertEqual(t.hello_ack.display_w, 122)
        self.assertEqual(t.hello_ack.display_h, 250)
        self.assertEqual(t.hello_ack.display_bpp_native, 1)
        self.assertEqual(t.hello_ack.boot_slot, 2)
        self.assertEqual(t.hello_ack.device_id, b"\x01" * 8)

        # Exactly one HELLO was written.
        hello_frames = [f for f in serial.written_frames if f.type == proto.Type.HELLO]
        self.assertEqual(len(hello_frames), 1)

    def test_hello_failure_raises_after_retries(self):
        clock = ManualClock()
        serial = ScriptedSerial(responder=silent_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=lambda device: serial,
            hello_timeout_s=0.05,
            hello_retries=3,
            clock=clock.time,
            sleep=clock.sleep,
        )
        with self.assertRaises(transport.HandshakeFailed):
            t.connect(do_hello=True)

        # One HELLO write per retry attempt.
        hello_frames = [f for f in serial.written_frames if f.type == proto.Type.HELLO]
        self.assertEqual(len(hello_frames), 3)

    def test_hello_nack_raises_handshake_failed(self):
        def nack_responder(frame: proto.Frame):
            if frame.type == proto.Type.HELLO:
                payload = struct.pack("<BBH", proto.ErrorCode.E_BAD_VERSION, 0, frame.seq)
                return proto.encode_frame(proto.Type.NACK, proto.Flags.IS_REPLY, frame.seq, payload)
            return None

        clock = ManualClock()
        serial = ScriptedSerial(responder=nack_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=lambda device: serial,
            hello_timeout_s=0.05,
            clock=clock.time,
            sleep=clock.sleep,
        )
        with self.assertRaises(transport.HandshakeFailed):
            t.connect(do_hello=True)


# --------------------------------------------------------------------------
# Reconnect / backoff
# --------------------------------------------------------------------------


class ReconnectTests(unittest.TestCase):
    def test_reconnect_backoff_retries_then_succeeds(self):
        clock = ManualClock()
        attempts = {"n": 0}

        def flaky_factory(device):
            attempts["n"] += 1
            if attempts["n"] < 3:
                raise OSError("device busy (simulated)")
            return ScriptedSerial(responder=hello_ack_responder)

        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=flaky_factory,
            reconnect_backoff_s=(0.1, 0.2, 0.4),
            clock=clock.time,
            sleep=clock.sleep,
        )
        t.connect_with_backoff(do_hello=True, max_attempts=5)

        self.assertEqual(attempts["n"], 3)
        self.assertIsNotNone(t.hello_ack)
        # Two failures -> two backoff sleeps, using the schedule in order.
        self.assertEqual(clock.sleep_calls, [0.1, 0.2])

    def test_reconnect_backoff_gives_up_after_max_attempts(self):
        clock = ManualClock()

        def always_fails(device):
            raise OSError("device busy (simulated)")

        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=always_fails,
            reconnect_backoff_s=(0.1,),
            clock=clock.time,
            sleep=clock.sleep,
        )
        with self.assertRaises(OSError):
            t.connect_with_backoff(do_hello=True, max_attempts=3)

    def test_backoff_schedule_repeats_last_value_past_its_length(self):
        clock = ManualClock()
        attempts = {"n": 0}

        def fails_four_times(device):
            attempts["n"] += 1
            if attempts["n"] < 5:
                raise OSError("simulated")
            return ScriptedSerial(responder=hello_ack_responder)

        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=fails_four_times,
            reconnect_backoff_s=(0.1, 0.2),
            clock=clock.time,
            sleep=clock.sleep,
        )
        t.connect_with_backoff(do_hello=True, max_attempts=10)
        # 4 failures -> schedule [0.1, 0.2, 0.2, 0.2] (last value repeats).
        self.assertEqual(clock.sleep_calls, [0.1, 0.2, 0.2, 0.2])


# --------------------------------------------------------------------------
# Request/reply plumbing (ACK/NACK, resync)
# --------------------------------------------------------------------------


class RequestReplyTests(unittest.TestCase):
    def test_request_returns_matching_ack(self):
        def ack_responder(frame: proto.Frame):
            return proto.encode_frame(proto.Type.ACK, proto.Flags.IS_REPLY, frame.seq)

        clock = ManualClock()
        serial = ScriptedSerial(responder=ack_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=lambda device: serial,
            clock=clock.time,
            sleep=clock.sleep,
        )
        t.connect(do_hello=False)
        reply = t.request(proto.Type.CLEAR, b"\x00")
        self.assertEqual(reply.type, proto.Type.ACK)
        self.assertTrue(reply.flags & proto.Flags.IS_REPLY)

    def test_request_ignores_leading_garbage_and_resyncs(self):
        # A device response preceded by noise on the wire must still parse
        # once the real frame's MAGIC is found (docs/protocol.md §7.7 HUNT).
        def noisy_ack_responder(frame: proto.Frame):
            garbage = b"\x00\x01garbage-before-magic"
            return garbage + proto.encode_frame(proto.Type.ACK, proto.Flags.IS_REPLY, frame.seq)

        clock = ManualClock()
        serial = ScriptedSerial(responder=noisy_ack_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=lambda device: serial,
            clock=clock.time,
            sleep=clock.sleep,
        )
        t.connect(do_hello=False)
        reply = t.request(proto.Type.CLEAR, b"\x00")
        self.assertEqual(reply.type, proto.Type.ACK)

    def test_request_times_out_and_raises_link_dead(self):
        clock = ManualClock()
        serial = ScriptedSerial(responder=silent_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=lambda device: serial,
            ack_timeout_s=0.02,
            clock=clock.time,
            sleep=clock.sleep,
        )
        t.connect(do_hello=False)
        with self.assertRaises(transport.LinkDead):
            t.request(proto.Type.GET_STATUS, retries=1)

    def test_request_waits_past_informational_e_seq_gap_nack(self):
        """Regression, incident 2026-09-04: docs/protocol.md §7.5/§9 --
        NACK/E_SEQ_GAP is informational, and the device processes the
        frame normally and sends the real reply right after it. Before
        this fix, request() treated that NACK as the reply itself, so a
        DRAW_BITMAP (etc.) sent right after some other process's HELLO
        reset the device's SEQ session would surface as a NackReceived
        and kill the caller's loop instead of just logging a warning and
        getting the real ACK."""

        def gap_then_ack_responder(frame: proto.Frame):
            nack_payload = struct.pack("<BBH", int(proto.ErrorCode.E_SEQ_GAP), 33, frame.seq)
            nack = proto.encode_frame(proto.Type.NACK, proto.Flags.IS_REPLY, frame.seq, nack_payload)
            ack = proto.encode_frame(proto.Type.ACK, proto.Flags.IS_REPLY, frame.seq)
            return nack + ack

        clock = ManualClock()
        serial = ScriptedSerial(responder=gap_then_ack_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=lambda device: serial,
            clock=clock.time,
            sleep=clock.sleep,
        )
        t.connect(do_hello=False)
        reply = t.request(proto.Type.DRAW_BITMAP, b"\x00")

        self.assertEqual(reply.type, proto.Type.ACK)
        # Exactly one DRAW_BITMAP was written -- the gap NACK was absorbed
        # within the same attempt, not mistaken for a timeout that would
        # have triggered a spurious retry (a second DRAW_BITMAP write).
        draw_bitmap_frames = [f for f in serial.written_frames if f.type == proto.Type.DRAW_BITMAP]
        self.assertEqual(len(draw_bitmap_frames), 1)

    def test_request_still_returns_a_non_gap_nack_immediately(self):
        """Only E_SEQ_GAP gets the wait-past treatment -- every other NACK
        code is still this request's reply, exactly as before."""

        def bad_param_nack_responder(frame: proto.Frame):
            payload = struct.pack("<BBH", int(proto.ErrorCode.E_BAD_PARAM), 2, frame.seq)
            return proto.encode_frame(proto.Type.NACK, proto.Flags.IS_REPLY, frame.seq, payload)

        clock = ManualClock()
        serial = ScriptedSerial(responder=bad_param_nack_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=lambda device: serial,
            clock=clock.time,
            sleep=clock.sleep,
        )
        t.connect(do_hello=False)
        reply = t.request(proto.Type.DRAW_TEXT, b"\x00")
        self.assertEqual(reply.type, proto.Type.NACK)

    def test_request_survives_a_malformed_short_nack_payload(self):
        """Regression: a NACK payload too short to hold the
        code/detail/seq_echo fields ("<BBH", 4 bytes per docs/protocol.md
        §7.5) must not crash request() itself with a bare struct.error --
        see the try/except around _decode_nack above. It falls through and
        returns the NACK frame as-is, leaving "what does this mean" to
        device.py's own, length-checked _decode_nack (device.py:57), which
        raises a clean, catchable ValueError instead."""

        def short_nack_responder(frame: proto.Frame):
            payload = b"\x00\x00"  # 2 bytes -- too short for "<BBH" (4 bytes)
            return proto.encode_frame(proto.Type.NACK, proto.Flags.IS_REPLY, frame.seq, payload)

        clock = ManualClock()
        serial = ScriptedSerial(responder=short_nack_responder)
        t = transport.SerialTransport(
            device="/dev/cu.usbmodemFAKE",
            comports=FAKE_MATCHING_COMPORTS,
            serial_factory=lambda device: serial,
            clock=clock.time,
            sleep=clock.sleep,
        )
        t.connect(do_hello=False)
        reply = t.request(proto.Type.DRAW_TEXT, b"\x00")
        self.assertEqual(reply.type, proto.Type.NACK)
        self.assertEqual(reply.payload, b"\x00\x00")


if __name__ == "__main__":
    unittest.main()
