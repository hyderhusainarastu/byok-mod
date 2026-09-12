"""Serial transport for the BYOK Link protocol.

Handles port discovery, connect/reconnect with backoff, and the HELLO
handshake. Framing itself (encode, incremental decode, resync per
docs/protocol.md §7.7) is `byok.proto`'s job — this module feeds bytes into
a `proto.Parser` and drives request/reply timing on top of it.

**Port safety.** The BYOK device exposes an unrelated USB-Serial-JTAG node for
~4 s at boot (see docs/hardware.md), and the stock firmware's own USB
personality can also enumerate a `/dev/cu.usbmodem*` node. This module must
never open a port on device-node pattern alone — it opens a candidate only
after `list_ports.comports()` reports a USB **product string** matching
`PRODUCT_STRING_MATCH`, which our firmware sets and the stock firmware does
not. If nothing matches, no port is opened, full stop.
"""

from __future__ import annotations

import logging
import os
import struct
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable, Iterable, List, Optional

from . import proto

logger = logging.getLogger("byok.transport")

# Our firmware's TinyUSB CDC-ACM product string (see docs/protocol.md §2 and
# firmware/byok_usb_cdc). Matched with a simple substring test, case-sensitive,
# against pyserial's ListPortInfo.product / .description.
PRODUCT_STRING_MATCH = "BYOK Mod Display"

DEFAULT_HELLO_TIMEOUT_S = 1.0
DEFAULT_HELLO_RETRIES = 3
DEFAULT_ACK_TIMEOUT_S = 0.25
DEFAULT_RECONNECT_BACKOFF_S = (0.5, 1.0, 2.0, 4.0, 8.0)  # last value repeats


class TransportError(Exception):
    """Base for transport-layer failures."""


class NoDeviceFound(TransportError):
    """No serial port with a matching product string was found."""


class WrongDevice(TransportError):
    """An explicit --port was given but it does not match our product
    string, i.e. it is not a device running our firmware. Refused rather
    than opened — see module docstring and SerialTransport._resolve_device."""


class PortBusy(TransportError):
    """The port was found and matched our product string, but another
    process already holds it open.

    Raised when `_default_serial_factory`'s `exclusive=True` open fails
    because a second `flock` on the same device node was refused (see
    `_is_port_busy_exception` and its docstring for exactly how this is
    detected). Before `exclusive=True` was added (incident 2026-09-04,
    see docs/design-rationale.md D-021 and docs/protocol.md §7.5), a second
    process could open the same port silently: it would send its own
    HELLO, which the device treats as a brand-new session
    (`byok_parser_new_session`), wiping out whatever SEQ state the
    *first* process's connection had already established -- the first
    process's very next frame would then land far ahead of the device's
    new expectation and draw an (informational, but until this fix
    fatal-by-omission) `NACK/E_SEQ_GAP`. `PortBusy` gives callers
    (`cli.py`'s `cmd_notify`) a way to distinguish "another process owns
    this" from "no device at all" (`NoDeviceFound`) or "wrong firmware"
    (`WrongDevice`), so they can fall back to the IPC request file
    (`byok.notify_ipc`) instead of ever attempting the second open."""


class HandshakeFailed(TransportError):
    """HELLO was sent but no valid HELLO_ACK arrived within the retry budget."""


class MockRequired(TransportError):
    """BYOK_FORCE_MOCK is set in the environment but no `serial_factory`
    override was supplied, i.e. this call would have opened a real serial
    port. Refused rather than opened.

    This exists as a safety net for dev/CI shells that must never touch
    the physical device (see SAFETY.md §1 and the project's hard rule
    "never open a real serial port (mock only)"). It does not change
    behavior for normal end-user CLI use, where BYOK_FORCE_MOCK is unset.
    Export BYOK_FORCE_MOCK=1 in any shell/session that should be
    hardware-blind, and pass an explicit `serial_factory` (as the test
    suite does) to talk to a fake device instead.
    """


class LinkDead(TransportError):
    """The link was open but stopped responding; caller should reconnect."""


@dataclass
class PortCandidate:
    device: str
    product: Optional[str]
    description: Optional[str]
    vid: Optional[int]
    pid: Optional[int]
    serial_number: Optional[str]


def _candidate_matches(c: PortCandidate) -> bool:
    for field in (c.product, c.description):
        if field and PRODUCT_STRING_MATCH in field:
            return True
    return False


def list_candidates(comports: Optional[Iterable] = None) -> List[PortCandidate]:
    """List /dev/cu.usbmodem* (or usbserial) ports, as PortCandidate,
    regardless of match. `comports` is injectable for tests; defaults to
    `serial.tools.list_ports.comports()`."""
    if comports is None:
        from serial.tools import list_ports

        comports = list_ports.comports()

    out: List[PortCandidate] = []
    for p in comports:
        device = getattr(p, "device", "") or ""
        if "usbmodem" not in device and "usbserial" not in device:
            # Only ever consider USB CDC nodes — never a Bluetooth or other
            # incidental /dev/cu.* / /dev/tty.* entry.
            continue
        out.append(
            PortCandidate(
                device=device,
                product=getattr(p, "product", None),
                description=getattr(p, "description", None),
                vid=getattr(p, "vid", None),
                pid=getattr(p, "pid", None),
                serial_number=getattr(p, "serial_number", None),
            )
        )
    return out


def find_device_port(comports: Optional[Iterable] = None) -> Optional[PortCandidate]:
    """Return the single candidate whose product string matches, or None.

    Never returns a candidate that did not match — see module docstring.
    """
    matches = [c for c in list_candidates(comports) if _candidate_matches(c)]
    if not matches:
        return None
    if len(matches) > 1:
        logger.warning(
            "multiple ports matched product string %r: %s — using the first",
            PRODUCT_STRING_MATCH,
            [c.device for c in matches],
        )
    return matches[0]


# --------------------------------------------------------------------------
# HELLO / HELLO_ACK payloads (docs/protocol.md §6.1)
#
# byok.proto is deliberately framing-only (see its module docstring); the
# handshake payload is small and handshake-specific enough to decode right
# here rather than invent a separate message-payload module just for it.
# The rest of the message set (INFO, STATUS, CLEAR, DRAW_*, FRAME_*, ...)
# is device.py's concern.
# --------------------------------------------------------------------------

_HELLO_STRUCT = "<BBHI"  # proto_ver_min, proto_ver_max, host_max_payload, host_caps
_HELLO_ACK_STRUCT = "<BBBBHHBBHIBBHI"  # 24 bytes; device_id (8 B) follows raw
_HELLO_ACK_FIXED_LEN = struct.calcsize(_HELLO_ACK_STRUCT)
_HELLO_ACK_LEN = _HELLO_ACK_FIXED_LEN + 8
_NACK_STRUCT = "<BBH"  # code, detail, seq_echo


@dataclass
class HelloAck:
    proto_ver: int
    fw_major: int
    fw_minor: int
    fw_patch: int
    display_w: int
    display_h: int
    display_bpp_native: int
    display_bpp_max: int
    dev_max_payload: int
    caps: int
    boot_slot: int
    is_our_firmware: int
    fonts: int
    uptime_ms: int
    device_id: bytes


def _encode_hello(proto_ver_min: int = 1, proto_ver_max: int = 1,
                   host_max_payload: int = proto.MAX_PAYLOAD, host_caps: int = 0) -> bytes:
    return struct.pack(_HELLO_STRUCT, proto_ver_min, proto_ver_max, host_max_payload, host_caps)


def _decode_hello_ack(payload: bytes) -> HelloAck:
    if len(payload) != _HELLO_ACK_LEN:
        raise HandshakeFailed(
            f"HELLO_ACK payload is {len(payload)} bytes, expected {_HELLO_ACK_LEN}"
        )
    fields = struct.unpack_from(_HELLO_ACK_STRUCT, payload, 0)
    device_id = bytes(payload[_HELLO_ACK_FIXED_LEN:_HELLO_ACK_LEN])
    return HelloAck(*fields, device_id=device_id)


def _decode_nack(payload: bytes):
    """Returns (code, detail, seq_echo)."""
    return struct.unpack_from(_NACK_STRUCT, payload, 0)


def _error_name(code: int) -> str:
    try:
        return proto.ErrorCode(code).name
    except ValueError:
        return f"0x{code:02X}"


def _is_port_busy_exception(exc: OSError) -> bool:
    """True if `exc` is pyserial's exclusive-lock failure -- posix
    backend, `serialposix.py`'s `Serial.open()`: `fcntl.flock(self.fd,
    LOCK_EX | LOCK_NB)` raising `OSError` gets wrapped as
    `SerialException(errno, "Could not exclusively lock port {}: {}")`.
    `SerialException` is an `OSError` subclass, so this only needs to
    check the message text, not `isinstance(exc, serial.SerialException)`
    -- which keeps this importable/checkable without a hard `serial`
    import, and lets a test's fake `serial_factory` raise a plain
    `OSError` with the same wording rather than a real
    `serial.SerialException`."""
    return "exclusively lock" in str(exc).lower()


class SerialTransport:
    """Frames a byte stream over a pyserial-like object.

    `serial_factory(device) -> serial_like` is injectable so tests can pass a
    fake serial object without touching a real port. The fake need only
    implement `.read(n)`, `.write(bytes)`, `.close()`, and a truthy
    `.is_open` (or no attribute at all, in which case it's assumed open after
    construction).
    """

    def __init__(
        self,
        device: Optional[str] = None,
        serial_factory: Optional[Callable[[str], object]] = None,
        comports: Optional[Iterable] = None,
        ack_timeout_s: float = DEFAULT_ACK_TIMEOUT_S,
        hello_timeout_s: float = DEFAULT_HELLO_TIMEOUT_S,
        hello_retries: int = DEFAULT_HELLO_RETRIES,
        reconnect_backoff_s: Iterable[float] = DEFAULT_RECONNECT_BACKOFF_S,
        sleep: Callable[[float], None] = time.sleep,
        clock: Callable[[], float] = time.monotonic,
        allow_unmatched_port: bool = False,
    ):
        self._explicit_device = device
        # Escape hatch for an explicit --port whose product string does not
        # match ours. Never set this from the ordinary --port flag — see
        # WrongDevice and _resolve_device below. Kept off by default so the
        # module docstring's invariant ("never open a port on device-node
        # pattern alone") actually holds for every normal code path.
        self._allow_unmatched_port = allow_unmatched_port
        self._serial_factory = serial_factory or self._default_serial_factory
        self._factory_overridden = serial_factory is not None
        self._comports = comports
        self._ack_timeout_s = ack_timeout_s
        self._hello_timeout_s = hello_timeout_s
        self._hello_retries = hello_retries
        self._reconnect_backoff_s = tuple(reconnect_backoff_s)
        self._sleep = sleep
        self._clock = clock

        self._ser = None
        self._device: Optional[str] = None
        self._parser: Optional[proto.Parser] = None
        self._frame_queue: "deque[proto.Frame]" = deque()
        self._seq = 0
        self.hello_ack: Optional[HelloAck] = None

    @staticmethod
    def _default_serial_factory(device: str):
        import serial

        # Baud rate is irrelevant on CDC (docs/protocol.md §2); any legal
        # value works. DTR must not be toggled to reset the device: pyserial
        # asserts DTR/RTS on open by default (dsrdtr/rtscts flow control),
        # which is the exact line-state combination the ESP32-S3's
        # USB-Serial-JTAG interprets as reset-to-bootloader. Disable both
        # flow-control lines and drive them low explicitly before any write.
        #
        # `exclusive=True` takes a POSIX `flock` on the device node (pyserial's
        # posix backend) so a second process opening the same port fails
        # loudly (`PortBusy`, via `_is_port_busy_exception` in `connect()`)
        # instead of silently succeeding. Before this, two host processes
        # could both hold the port: the second's HELLO would wipe the
        # first's SEQ session on the device (incident 2026-09-04, see
        # `PortBusy`'s own docstring). This is the actual single-consumer
        # invariant the rest of this module's docstring already assumed.
        ser = serial.Serial(
            device,
            baudrate=115200,
            timeout=0,
            write_timeout=1.0,
            dsrdtr=False,
            rtscts=False,
            exclusive=True,
        )
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            # Some backends/mocks may not support these; opening still
            # succeeded and dsrdtr/rtscts=False already avoids the assert.
            pass
        return ser

    # -- discovery / connection -------------------------------------------

    def _resolve_device(self) -> str:
        if self._explicit_device:
            matches = {c.device: c for c in list_candidates(self._comports)}
            candidate = matches.get(self._explicit_device)
            if candidate is not None and _candidate_matches(candidate):
                return self._explicit_device
            if self._allow_unmatched_port:
                logger.warning(
                    "opening %s WITHOUT a matching product string (allow_unmatched_port) "
                    "-- this may be the stock firmware's port or an unrelated device",
                    self._explicit_device,
                )
                return self._explicit_device
            raise WrongDevice(
                f"{self._explicit_device} does not report a product string containing "
                f"{PRODUCT_STRING_MATCH!r} -- refusing to open it. This is very likely "
                "not our firmware (it may be the stock firmware's port, or an unrelated "
                "device), and opening it can reset whatever is on the other end. If you "
                "are certain, pass --unsafe-port instead of --port."
            )
        candidate = find_device_port(self._comports)
        if candidate is None:
            raise NoDeviceFound(
                f"no serial port with product string containing {PRODUCT_STRING_MATCH!r} found"
            )
        return candidate.device

    def connect(self, do_hello: bool = True) -> None:
        """Open the port and, by default, perform the HELLO handshake."""
        if os.environ.get("BYOK_FORCE_MOCK") and not self._factory_overridden:
            raise MockRequired(
                "BYOK_FORCE_MOCK is set but no serial_factory override was given -- "
                "refusing to open a real serial port. See MockRequired's docstring."
            )
        device = self._resolve_device()
        logger.info("opening %s", device)
        try:
            self._ser = self._serial_factory(device)
        except OSError as exc:
            if _is_port_busy_exception(exc):
                raise PortBusy(
                    f"{device} is already open by another process (exclusive lock held) -- "
                    "see PortBusy's docstring"
                ) from exc
            raise
        self._device = device
        self._parser = proto.Parser(
            on_error=lambda err: logger.warning(
                "resync: %s (total resync_events=%d)", err.err.name, self._parser.resync_events
            )
        )
        self._frame_queue.clear()
        self._seq = 0
        self.hello_ack = None
        if do_hello:
            self.hello()

    def is_connected(self) -> bool:
        return self._ser is not None and getattr(self._ser, "is_open", True)

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:  # noqa: BLE001 - best-effort on teardown
                logger.debug("error closing serial port", exc_info=True)
        self._ser = None
        self._device = None

    def connect_with_backoff(self, do_hello: bool = True, max_attempts: Optional[int] = None) -> None:
        """Retry connect() with increasing backoff until it succeeds.

        `max_attempts=None` retries forever (intended for a long-lived CLI
        session); pass a positive int to bound it (tests do).
        """
        attempt = 0
        while True:
            attempt += 1
            try:
                self.connect(do_hello=do_hello)
                return
            except (TransportError, OSError) as exc:
                if max_attempts is not None and attempt >= max_attempts:
                    raise
                delay = self._reconnect_backoff_s[
                    min(attempt - 1, len(self._reconnect_backoff_s) - 1)
                ]
                logger.warning("connect attempt %d failed (%s); retrying in %.1fs", attempt, exc, delay)
                self.close()
                self._sleep(delay)

    # -- low-level I/O -------------------------------------------------------

    def _read_available(self, max_bytes: int = 4096) -> bytes:
        if self._ser is None:
            raise LinkDead("not connected")
        try:
            data = self._ser.read(max_bytes)
        except OSError as exc:
            raise LinkDead(f"read failed: {exc}") from exc
        return data or b""

    def _write(self, data: bytes) -> None:
        if self._ser is None:
            raise LinkDead("not connected")
        try:
            self._ser.write(data)
        except OSError as exc:
            raise LinkDead(f"write failed: {exc}") from exc

    def _next_seq(self) -> int:
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFFFF
        return seq

    def _read_frame(self, deadline: float) -> Optional[proto.Frame]:
        """Read (and let proto.Parser decode) until one frame is available,
        or `deadline` (monotonic, from self._clock()) passes."""
        if self._frame_queue:
            return self._frame_queue.popleft()
        while self._clock() < deadline:
            chunk = self._read_available()
            if chunk:
                frames, _errors = self._parser.feed(chunk)
                if frames:
                    self._frame_queue.extend(frames)
                    return self._frame_queue.popleft()
            else:
                self._sleep(0.005)
        return None

    # -- request/reply -------------------------------------------------------

    def send(self, msg_type: int, flags: int, payload: bytes = b"") -> int:
        """Send one frame, returning the SEQ used."""
        seq = self._next_seq()
        self._write(proto.encode_frame(int(msg_type), int(flags), seq, payload))
        return seq

    def request(
        self,
        msg_type: int,
        payload: bytes = b"",
        timeout_s: Optional[float] = None,
        retries: int = 2,
    ) -> proto.Frame:
        """Send with ACK_REQ set and wait for the matching reply.

        Retries the same SEQ on timeout, per docs/protocol.md §7.6, then
        raises LinkDead. Drops (and keeps waiting past) any EVENT frames or
        replies for a different SEQ that arrive first.

        A matching reply that is `NACK/E_SEQ_GAP` is *not* treated as this
        request's reply: docs/protocol.md §7.5/§9 -- that NACK is purely
        informational ("Gaps are reported, not repaired", and the receiver
        "processes the frame normally"), so the device still sends the real
        ACK/typed reply for this SEQ right after it. Returning the NACK here instead
        (as the code did before incident 2026-09-04 -- see PortBusy's
        docstring for the fuller incident writeup) makes an ordinary,
        harmless gap report look like the request itself failed. This is
        logged and waited past, within the same attempt's deadline, same as
        an EVENT frame. Any other NACK code *is* the reply, exactly as
        before -- only E_SEQ_GAP gets this treatment.
        """
        timeout_s = self._ack_timeout_s if timeout_s is None else timeout_s
        seq = self._next_seq()
        frame_bytes = proto.encode_frame(int(msg_type), int(proto.Flags.ACK_REQ), seq, payload)

        attempts = retries + 1
        for attempt in range(attempts):
            self._write(frame_bytes)
            deadline = self._clock() + timeout_s
            while True:
                reply = self._read_frame(deadline)
                if reply is None:
                    break  # timed out this attempt
                if reply.flags & proto.Flags.EVENT:
                    continue  # events are never replies; keep waiting
                if (reply.flags & proto.Flags.IS_REPLY) and reply.seq == seq:
                    if reply.type == proto.Type.NACK:
                        try:
                            code, detail, _seq_echo = _decode_nack(reply.payload)
                        except struct.error:
                            # Malformed NACK payload (too short to hold the
                            # code/detail/seq_echo fields) -- fall through
                            # and return it as-is so device.py's own,
                            # length-checked _decode_nack (device.py:57)
                            # raises the clean, catchable ValueError instead
                            # of this call dying on a bare struct.error.
                            return reply
                        if code == proto.ErrorCode.E_SEQ_GAP:
                            logger.warning(
                                "NACK E_SEQ_GAP (detail=%d frame(s) missed) for type=0x%02X "
                                "(seq=%d) -- informational (docs/protocol.md §7.5): the "
                                "device processes the frame normally and a real reply "
                                "follows; still waiting for it",
                                detail, msg_type, seq,
                            )
                            continue  # not the reply -- keep waiting for the real one
                    return reply
                # Reply to a stale request, or unrelated traffic — ignore.
            if attempt < attempts - 1:
                logger.debug("request type=0x%02X (seq=%d) timed out, retrying", msg_type, seq)
        raise LinkDead(f"no reply to type=0x{msg_type:02X} (seq={seq}) after {attempts} attempts")

    # -- unsolicited events --------------------------------------------

    def poll_events(self) -> List["proto.Frame"]:
        """Non-blocking: read whatever bytes are currently sitting in the
        OS's receive buffer (a real `serial.Serial` here is opened with
        `timeout=0`, so `.read()` never blocks -- see
        `_default_serial_factory`'s comment) and return any `FLAGS.EVENT`
        frames decoded from them, in arrival order.

        Intended to be called once per `dashboard/loop.py` cycle, between
        (not during) `request()` calls -- `request()` itself already drops
        EVENT frames silently while it waits for a specific reply
        (`_read_frame`'s "events are never replies; keep waiting" branch),
        so this is the only path that actually surfaces
        `EVT_BUTTON`/`EVT_STATUS`/`EVT_LOG`/`PRESET_CHANGED` to a caller.
        Any *non*-event frame decoded here (there normally shouldn't be
        one -- nothing should be sending the host an unsolicited reply)
        is requeued onto `_frame_queue` rather than dropped, so a
        `request()` called right after this still sees it. Also drains
        `_frame_queue` of any event frames already sitting there (there
        normally won't be any: `request()`'s own read loop decodes and
        discards EVENT frames itself while it waits for a specific reply
        -- docs/protocol.md §7.4 "Events are never acknowledged" means
        this is an accepted, documented gap, not a bug being routed
        around here -- so an event landing exactly inside a `request()`'s
        wait window is simply missed, same as protocol.md says a host
        that misses one is expected to be).

        Returns `[]` (never raises) if the transport isn't connected --
        callers that poll every cycle regardless of link state (the
        dashboard loop does) shouldn't need their own guard for that.
        """
        if self._ser is None or self._parser is None:
            return []
        events: List[proto.Frame] = []
        chunk = self._read_available()
        if chunk:
            frames, _errors = self._parser.feed(chunk)
            for f in frames:
                if f.flags & proto.Flags.EVENT:
                    events.append(f)
                else:
                    self._frame_queue.append(f)
        # Also drain any events a request()'s own wait already decoded and
        # left in the queue (it never consumes EVENT frames from there --
        # see _read_frame -- it only reads new bytes and re-checks type).
        remaining: "deque[proto.Frame]" = deque()
        while self._frame_queue:
            f = self._frame_queue.popleft()
            if f.flags & proto.Flags.EVENT:
                events.append(f)
            else:
                remaining.append(f)
        self._frame_queue = remaining
        return events

    # -- handshake -------------------------------------------------------

    def hello(self) -> HelloAck:
        payload = _encode_hello(host_max_payload=proto.MAX_PAYLOAD)
        last_exc: Optional[Exception] = None
        for _attempt in range(self._hello_retries):
            try:
                reply = self.request(
                    proto.Type.HELLO, payload, timeout_s=self._hello_timeout_s, retries=0
                )
            except LinkDead as exc:
                last_exc = exc
                continue
            if reply.type == proto.Type.NACK:
                code, _detail, _seq_echo = _decode_nack(reply.payload)
                raise HandshakeFailed(f"device NACKed HELLO: {_error_name(code)}")
            if reply.type != proto.Type.HELLO_ACK:
                last_exc = HandshakeFailed(f"unexpected reply type to HELLO: 0x{reply.type:02X}")
                continue
            self.hello_ack = _decode_hello_ack(reply.payload)
            # connect() always builds a fresh Parser before the first HELLO
            # of a connection, so this is normally a no-op; kept so a
            # future caller that re-HELLOs on a live Parser still gets a
            # clean SEQ session (see proto.Parser.new_session docstring).
            if self._parser is not None:
                self._parser.new_session()
            logger.info(
                "HELLO_ACK: fw=%d.%d.%d display=%dx%d bpp_native=%d",
                self.hello_ack.fw_major, self.hello_ack.fw_minor, self.hello_ack.fw_patch,
                self.hello_ack.display_w, self.hello_ack.display_h,
                self.hello_ack.display_bpp_native,
            )
            return self.hello_ack
        raise HandshakeFailed(f"no HELLO_ACK after {self._hello_retries} attempts") from last_exc
