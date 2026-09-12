"""Shared serial-port fakes for tests that need a live `byok.device.Device`
without ever touching a real port -- same technique test_transport.py uses
(a fake object satisfying `.read`/`.write`/`.close`/`.is_open`, decoding
outbound bytes with the real `byok.proto.Parser`), generalized to answer
*any* ACK_REQ'd BYOK Link request generically rather than scripting one
exchange at a time. Used by test_dashboard_loop.py and test_device_image.py.

No real serial port is ever opened here.
"""

from __future__ import annotations

import os
import struct
import sys

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_HOST_MACOS = os.path.join(_REPO_ROOT, "host", "macos")
if _HOST_MACOS not in sys.path:
    sys.path.insert(0, _HOST_MACOS)

from byok import proto  # noqa: E402

# A device node + product string pair that transport._resolve_device()
# (and find_device_port()) will accept -- see transport.py's module
# docstring on why matching is required even for an explicit --port.
FAKE_DEVICE_PATH = "/dev/cu.usbmodemFAKE"


class ManualClock:
    """A fake monotonic clock/sleep pair with no real wall-time cost --
    `sleep(s)` advances the clock instead of blocking. Identical in spirit
    to test_transport.py's own ManualClock (kept separate to avoid a
    cross-test-file import)."""

    def __init__(self) -> None:
        self.t = 0.0
        self.sleep_calls = []

    def time(self) -> float:
        return self.t

    def sleep(self, seconds: float) -> None:
        self.sleep_calls.append(seconds)
        self.t += seconds


def make_port_info(device: str, product=None, description=None):
    import types

    ns = types.SimpleNamespace()
    ns.device = device
    ns.product = product
    ns.description = description
    ns.vid = None
    ns.pid = None
    ns.serial_number = None
    return ns


FAKE_MATCHING_COMPORTS = [
    make_port_info(FAKE_DEVICE_PATH, product="BYOK Mod Display", description="BYOK Mod Display")
]


_HELLO_ACK_STRUCT = "<BBBBHHBBHIBBHI"


def hello_ack_payload(
    display_w: int = 240,
    display_h: int = 80,
    bpp_native: int = 1,
    bpp_max: int = 2,
    caps: int = 0x010F,  # bit0 partial refresh, bit1 2bpp, bit8 stock image present
    device_id: bytes = b"\x01" * 8,
) -> bytes:
    return (
        struct.pack(
            _HELLO_ACK_STRUCT,
            1,  # proto_ver
            1, 2, 3,  # fw_major, fw_minor, fw_patch
            display_w, display_h,
            bpp_native, bpp_max,
            4096,  # dev_max_payload
            caps,
            2,  # boot_slot
            1,  # is_our_firmware
            3,  # fonts
            12345,  # uptime_ms
        )
        + device_id
    )


class RecordingSerial:
    """Fake serial port: decodes every outbound frame with a real
    `byok.proto.Parser` (recording each one in `written_frames`, in wire
    order) and, unless the frame's TYPE is in `silent_types`, answers every
    ACK_REQ'd request with a generic reply -- HELLO gets a real HELLO_ACK
    (`hello_payload`), everything else gets an empty ACK. `silent_types`
    models a link that has gone dead for specific message types (what a
    disconnect looks like from the requesting side: the write succeeds,
    nothing ever answers it).
    """

    def __init__(self, silent_types=frozenset(), hello_payload: bytes = None, type_replies=None):
        self.is_open = True
        self._rx = bytearray()
        self.written_frames = []
        self._silent_types = set(silent_types)
        self._hello_payload = hello_payload if hello_payload is not None else hello_ack_payload()
        # `type_replies`: {request_type: (reply_type, payload) | callable(frame) -> (reply_type, payload)}.
        # Lets a test script a specific typed reply (e.g. GET_DOCSTATS ->
        # DOCSTATS) instead of every ACK_REQ'd request getting a generic
        # empty ACK -- see writing.py's DeviceDocStatsProvider tests.
        self._type_replies = dict(type_replies or {})
        self._parser = proto.Parser(on_frame=self._on_frame)

    def _on_frame(self, frame: proto.Frame) -> None:
        self.written_frames.append(frame)
        if frame.type in self._silent_types:
            return
        if not (frame.flags & proto.Flags.ACK_REQ):
            return  # e.g. FRAME_DATA, which is fire-and-forget
        if frame.type == proto.Type.HELLO:
            reply = proto.encode_frame(proto.Type.HELLO_ACK, proto.Flags.IS_REPLY, frame.seq, self._hello_payload)
        elif frame.type in self._type_replies:
            scripted = self._type_replies[frame.type]
            reply_type, payload = scripted(frame) if callable(scripted) else scripted
            reply = proto.encode_frame(reply_type, proto.Flags.IS_REPLY, frame.seq, payload)
        else:
            reply = proto.encode_frame(proto.Type.ACK, proto.Flags.IS_REPLY, frame.seq, b"")
        self._rx.extend(reply)

    def push_event(self, msg_type: int, payload: bytes = b"") -> None:
        """Injects an unsolicited device->host EVENT frame directly into
        the read buffer, as if the device had just emitted it (button
        press, PRESET_CHANGED, ...) -- for `SerialTransport.poll_events()`
        tests. Uses the device's own SEQ space starting at 0, which is
        fine: EVENT frames are never matched against a request SEQ."""
        self._rx.extend(proto.encode_frame(msg_type, proto.Flags.EVENT, 0, payload))

    def write(self, data: bytes) -> int:
        self._parser.feed(data)
        return len(data)

    def read(self, n: int = 1) -> bytes:
        chunk = bytes(self._rx[:n])
        del self._rx[: len(chunk)]
        return chunk

    def close(self) -> None:
        self.is_open = False


def connected_device(
    *,
    clock: ManualClock = None,
    ack_timeout_s: float = 0.02,
    hello_timeout_s: float = 0.02,
    reconnect_backoff_s=(0.01, 0.02, 0.04),
    serial_factory=None,
    hello_payload: bytes = None,
    type_replies=None,
):
    """Build a `Device` wrapping a `SerialTransport` that has already
    completed the HELLO handshake against a fresh `RecordingSerial`, with a
    `ManualClock` driving all timeouts/backoff so nothing here costs real
    wall-clock time. Returns `(device, serials, clock)` -- `serials` is a
    `{"serial": <most recently (re)connected RecordingSerial>}` dict, live
    across any later reconnect (read `serials["serial"]` fresh each time,
    don't snapshot it before a reconnect), `clock` is the `ManualClock` in
    use (same as `clock=` if passed in).

    `serial_factory(device_path) -> RecordingSerial` defaults to a fresh,
    fully-responsive `RecordingSerial` every call (each (re)connect gets its
    own instance, matching what a real reconnect does) -- pass your own to
    script a mid-session failure.
    """
    from byok.device import Device
    from byok.transport import SerialTransport

    clock = clock or ManualClock()
    holder = {}

    def default_factory(_device_path):
        return RecordingSerial(hello_payload=hello_payload, type_replies=type_replies)

    user_factory = serial_factory or default_factory

    def factory(device_path):
        # Wrap whichever factory is in play so `holder["serial"]` always
        # reflects the most recently (re)connected instance, including
        # across a reconnect with a caller-supplied scripted factory.
        s = user_factory(device_path)
        holder["serial"] = s
        return s

    transport = SerialTransport(
        device=FAKE_DEVICE_PATH,
        comports=FAKE_MATCHING_COMPORTS,
        serial_factory=factory,
        ack_timeout_s=ack_timeout_s,
        hello_timeout_s=hello_timeout_s,
        reconnect_backoff_s=reconnect_backoff_s,
        clock=clock.time,
        sleep=clock.sleep,
    )
    transport.connect(do_hello=True)
    device = Device(transport)
    return device, holder, clock
