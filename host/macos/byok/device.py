"""High-level device API: info, status, clear, text, image, refresh.

Wraps a `transport.SerialTransport` and speaks in Python types rather than
raw frames — this is the layer `cli.py` (and, later, `dash.py` / a mirror
loop) is written against.

`byok.proto` is deliberately framing-only (see its module docstring — it
builds/parses the header+payload+CRC envelope but does not know what any
given TYPE's payload means). This module is the "dispatch layer" that
docstring defers to: the `_encode_*` / `_decode_*` helpers below implement
the payload layouts of docs/protocol.md §6 for exactly the messages this
API needs.
"""

from __future__ import annotations

import logging
import struct
from datetime import datetime, timezone
from typing import Optional, Tuple

from PIL import Image

from . import proto, render
from .transport import SerialTransport, TransportError

logger = logging.getLogger("byok.device")

DEFAULT_FRAME_CHUNK = 4092  # docs/protocol.md §8: max FRAME_DATA chunk
FULL_REFRESH_MODE = 2
PARTIAL_REFRESH_MODE = 1
NO_REFRESH_MODE = 0
DEVICE_CHOOSES_REFRESH_MODE = 3


class NackReceived(Exception):
    """Raised when the device answers a request with NACK."""

    def __init__(self, code: int, detail: int, seq_echo: int):
        try:
            name = proto.ErrorCode(code).name
        except ValueError:
            name = f"0x{code:02X}"
        super().__init__(f"NACK {name} (detail={detail}, seq_echo={seq_echo})")
        self.code = code
        self.detail = detail
        self.seq_echo = seq_echo


# --------------------------------------------------------------------------
# §6 payload layouts this API needs. All integers little-endian, per §6.
# --------------------------------------------------------------------------


def _decode_nack(payload: bytes) -> Tuple[int, int, int]:
    if len(payload) != 4:
        raise ValueError(f"NACK payload is {len(payload)} bytes, expected 4")
    return struct.unpack_from("<BBH", payload, 0)


def _decode_info(payload: bytes) -> "Info":
    if len(payload) != 64:
        raise ValueError(f"INFO payload is {len(payload)} bytes, expected 64")
    fw_version = payload[0:16].rstrip(b"\x00").decode("utf-8", "replace")
    idf_version = payload[16:32].rstrip(b"\x00").decode("utf-8", "replace")
    build_date = payload[32:48].rstrip(b"\x00").decode("utf-8", "replace")
    flash_size, psram_size, cpu_hz, caps = struct.unpack_from("<IIII", payload, 48)
    return Info(fw_version, idf_version, build_date, flash_size, psram_size, cpu_hz, caps)


def _decode_status(payload: bytes) -> "Status":
    if len(payload) != 20:
        raise ValueError(f"STATUS payload is {len(payload)} bytes, expected 20")
    battery_mv, battery_pct, charging, mode, backlight, contrast, flags = (
        struct.unpack_from("<HBBBBBB", payload, 0)
    )
    uptime_ms, free_heap, min_free_heap = struct.unpack_from("<III", payload, 8)
    return Status(battery_mv, battery_pct, charging, mode, backlight, contrast,
                  flags, uptime_ms, free_heap, min_free_heap)


def _encode_clear(value: int) -> bytes:
    return struct.pack("<B", value & 0x01)


def _encode_draw_text(x: int, y: int, font_id: int, style: int, text: bytes) -> bytes:
    if len(text) > 4088:
        raise ValueError("DRAW_TEXT text exceeds 4088 bytes")
    return struct.pack("<HHBBH", x, y, font_id, style, len(text)) + text


def _encode_draw_rect(x: int, y: int, w: int, h: int, op: int, value: int) -> bytes:
    return struct.pack("<HHHHBB", x, y, w, h, op, value)


def _encode_draw_bitmap(x: int, y: int, w: int, h: int, bpp: int, op: int, pixels: bytes) -> bytes:
    return struct.pack("<HHHHBB", x, y, w, h, bpp, op) + pixels


def _encode_set_backlight(level: int, fade_ms: int) -> bytes:
    return struct.pack("<BH", level & 0xFF, min(fade_ms, 5000))


def _encode_set_contrast(value: int) -> bytes:
    return struct.pack("<B", value & 0xFF)


def _encode_set_mode(mode: int, flags: int = 0) -> bytes:
    return struct.pack("<BB", mode, flags)


def _encode_reboot() -> bytes:
    return struct.pack("<H", 0x5245)  # 'RE'


def _encode_boot_original() -> bytes:
    return struct.pack("<I", 0x424B5354)  # 'BKST'


def _encode_set_time(year: int, month: int, day: int, weekday: int,
                      hour: int, minute: int, second: int) -> bytes:
    """§6.4 SET_TIME (8 B, v1.1): u16 year, then u8 month/day/weekday/
    hour/minute/second, all little-endian. `year` is the full year
    (2000-2199), not 2-digit. `weekday` is opaque on the wire (0-6, the
    device round-trips it without assigning it a meaning of its own) — see
    Device.set_time()'s own docstring for what this module sends there.
    Field ranges are not checked here; the device validates independently
    and NACKs (E_BAD_PARAM) with the offending field's byte offset."""
    return struct.pack("<HBBBBBB", year, month, day, weekday, hour, minute, second)


def _decode_docstats(payload: bytes) -> "DocStats":
    """docs/protocol.md §6.4b `DOCSTATS` (0x2B). 20 B: u32 files, words,
    bytes, newest_epoch, words_today, all LE -- reconciled: this field
    order came straight from the firmware-side definition and needed no
    correction; only the TYPE code moved (0x2A -> 0x2B) during
    reconciliation -- see proto.py's Type enum comment."""
    if len(payload) != 20:
        raise ValueError(f"DOCSTATS payload is {len(payload)} bytes, expected 20")
    files, words, doc_bytes, newest_epoch, words_today = struct.unpack_from("<IIIII", payload, 0)
    return DocStats(files, words, doc_bytes, newest_epoch, words_today)


# docs/protocol.md §6.4b: SET_PRESETS supports at most 8 presets (a menu
# row limit), each name a fixed 20-byte NUL-padded field -- reconciled
# from an earlier host-side draft design (variable-length-prefixed
# names, up to 255 of them), which encoded nothing compatible with this.
SET_PRESETS_MAX_COUNT = 8
_PRESET_NAME_LEN = 20


def _encode_set_presets(names) -> bytes:
    """docs/protocol.md §6.4b `SET_PRESETS` payload: `u8 count` (0-8) then
    `count` fixed 20-byte NUL-padded name fields, in menu order. `LEN`
    must equal exactly `1 + count*20` (the device NACKs `E_BAD_LENGTH`
    otherwise, per §6.4b's own note that a short/long trailing name would
    silently corrupt every name after it) -- this function always emits
    exactly that many bytes. Each name is UTF-8-encoded and truncated to
    20 bytes if longer (`byok.dashboard.presets.MAX_DISPLAY_NAME_LEN`
    already enforces this at the manifest-validation level, so a
    well-formed manifest never hits the truncation path here; this is a
    second, wire-level backstop). Raises ValueError if there are more
    than `SET_PRESETS_MAX_COUNT` (8) presets."""
    names = list(names)
    if len(names) > SET_PRESETS_MAX_COUNT:
        raise ValueError(
            f"SET_PRESETS supports at most {SET_PRESETS_MAX_COUNT} presets, got {len(names)}"
        )
    out = bytearray([len(names)])
    for name in names:
        encoded = name.encode("utf-8")[:_PRESET_NAME_LEN]
        out.extend(encoded)
        out.extend(b"\x00" * (_PRESET_NAME_LEN - len(encoded)))
    return bytes(out)


# docs/protocol.md §6.4b `DISPLAY_CFG` payload: `u8 flags`, bit0 = use
# bulk I2C writes, bits 1-7 reserved (send 0) -- reconciled from this
# pass's own first-guess two-byte `op, value` design.
DISPLAY_CFG_FLAG_BULK = 0x01


def _encode_display_cfg(bulk: bool) -> bytes:
    return struct.pack("<B", DISPLAY_CFG_FLAG_BULK if bulk else 0)


def _decode_evt_preset_changed(payload: bytes) -> int:
    """docs/protocol.md §6.5 `EVT_PRESET_CHANGED` (0x33): `u8 index`,
    the newly-confirmed preset index -- reconciled from an earlier
    host-side draft's `u16` payload. Fired once per completed menu selection
    (an explicit EXECUTE confirm), never on the 10s no-selection
    timeout."""
    if len(payload) != 1:
        raise ValueError(f"EVT_PRESET_CHANGED payload is {len(payload)} bytes, expected 1")
    return payload[0]


# docs/protocol.md §6.1 STATUS/EVT_STATUS `flags` byte (unchanged size,
# newly-meaningful bits as of v1.2/0.1.14): bits 4-6 the currently
# selected preset index (0-7), bit 7 whether the preset menu is
# currently open. Reserved-bit semantics were already receiver-must-
# ignore per §4, so a v1.0/v1.1-only host reading this field is
# unaffected; these two helpers are what let a v1.2-aware host (this
# one) actually use the newly-defined bits.
STATUS_FLAG_MENU_OPEN = 0x80


def preset_index_from_status_flags(flags: int) -> int:
    return (flags >> 4) & 0x07


def menu_open_from_status_flags(flags: int) -> bool:
    return bool(flags & STATUS_FLAG_MENU_OPEN)


def _decode_evt_button(payload: bytes) -> "ButtonEvent":
    """docs/protocol.md §6.5 `EVT_BUTTON` (6 B): u8 button, u8 state, u32
    t_ms. Not a PROPOSED type -- this one is already fully specified;
    only the decode helper is new (nothing in this tree read EVT_BUTTON
    before v1.2)."""
    if len(payload) != 6:
        raise ValueError(f"EVT_BUTTON payload is {len(payload)} bytes, expected 6")
    button, state, t_ms = struct.unpack_from("<BBI", payload, 0)
    return ButtonEvent(button, state, t_ms)


# docs/protocol.md §6.5 EVT_BUTTON.button values.
BUTTON_UP = 1
BUTTON_DOWN = 2
BUTTON_EXECUTE = 3
BUTTON_BRIGHTNESS = 4
BUTTON_WAKE = 5

# .state values.
BUTTON_STATE_RELEASED = 0
BUTTON_STATE_PRESSED = 1
BUTTON_STATE_AUTO_REPEAT = 2
BUTTON_STATE_LONG_PRESS = 3


class DocStats:
    __slots__ = ("files", "words", "bytes", "newest_epoch", "words_today")

    def __init__(self, files, words, doc_bytes, newest_epoch, words_today):
        self.files = files
        self.words = words
        self.bytes = doc_bytes
        self.newest_epoch = newest_epoch
        self.words_today = words_today


class ButtonEvent:
    __slots__ = ("button", "state", "t_ms")

    def __init__(self, button, state, t_ms):
        self.button = button
        self.state = state
        self.t_ms = t_ms


def _encode_frame_begin(w: int, h: int, bpp: int, flags: int, origin_x: int = 0) -> bytes:
    return struct.pack("<HHBBH", w, h, bpp, flags, origin_x)


def _encode_frame_data(offset: int, chunk: bytes) -> bytes:
    if len(chunk) > 4092:
        raise ValueError("FRAME_DATA chunk exceeds 4092 bytes")
    return struct.pack("<I", offset) + chunk


def _encode_frame_end(frame_crc32: int, refresh: int) -> bytes:
    return struct.pack("<IB", frame_crc32, refresh)


def _encode_partial_refresh(x: int, y: int, w: int, h: int) -> bytes:
    return struct.pack("<HHHH", x, y, w, h)


def _encode_full_refresh(flags: int = 0) -> bytes:
    return struct.pack("<B", flags & 0xFF)


class Info:
    __slots__ = ("fw_version", "idf_version", "build_date", "flash_size", "psram_size", "cpu_hz", "caps")

    def __init__(self, fw_version, idf_version, build_date, flash_size, psram_size, cpu_hz, caps):
        self.fw_version = fw_version
        self.idf_version = idf_version
        self.build_date = build_date
        self.flash_size = flash_size
        self.psram_size = psram_size
        self.cpu_hz = cpu_hz
        self.caps = caps


class Status:
    __slots__ = (
        "battery_mv", "battery_pct", "charging", "mode", "backlight", "contrast",
        "flags", "uptime_ms", "free_heap", "min_free_heap",
    )

    def __init__(self, battery_mv, battery_pct, charging, mode, backlight, contrast,
                 flags, uptime_ms, free_heap, min_free_heap):
        self.battery_mv = battery_mv
        self.battery_pct = battery_pct
        self.charging = charging
        self.mode = mode
        self.backlight = backlight
        self.contrast = contrast
        self.flags = flags
        self.uptime_ms = uptime_ms
        self.free_heap = free_heap
        self.min_free_heap = min_free_heap


class Device:
    """High-level, blocking API over one connected BYOK Link device."""

    def __init__(self, transport: SerialTransport):
        self.transport = transport

    @classmethod
    def open(cls, port: Optional[str] = None, **transport_kwargs) -> "Device":
        """Discover (or use `port`) and connect, performing the HELLO handshake."""
        t = SerialTransport(device=port, **transport_kwargs)
        t.connect(do_hello=True)
        return cls(t)

    def close(self) -> None:
        self.transport.close()

    def __enter__(self) -> "Device":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    # -- geometry, from the handshake, never hard-coded (protocol.md §6.1) --

    @property
    def hello_ack(self):
        if self.transport.hello_ack is None:
            raise RuntimeError("not connected — call Device.open() or transport.connect() first")
        return self.transport.hello_ack

    @property
    def display_size(self) -> Tuple[int, int]:
        h = self.hello_ack
        return h.display_w, h.display_h

    @property
    def native_bpp(self) -> int:
        return self.hello_ack.display_bpp_native

    # -- internal: request + NACK-checked reply -----------------------------

    def _request(self, msg_type: int, payload: bytes = b"") -> proto.Frame:
        reply = self.transport.request(msg_type, payload)
        if reply.type == proto.Type.NACK:
            try:
                code, detail, seq_echo = _decode_nack(reply.payload)
            except ValueError as exc:
                # A malformed NACK (wrong payload length) is still a
                # protocol-level failure, not a decode bug worth crashing
                # the caller over -- surface it as TransportError so it
                # lands in the same (TransportError, NackReceived) recovery
                # path callers (dashboard/loop.py, mirror.py) already use
                # for a genuine NackReceived, instead of escaping as a bare
                # ValueError that neither one catches.
                raise TransportError(f"malformed NACK: {exc}") from exc
            raise NackReceived(code, detail, seq_echo)
        return reply

    # -- session / info -------------------------------------------------

    def info(self) -> Info:
        reply = self._request(proto.Type.GET_INFO)
        return _decode_info(reply.payload)

    def status(self) -> Status:
        reply = self._request(proto.Type.GET_STATUS)
        return _decode_status(reply.payload)

    def ping(self, cookie: Optional[int] = None) -> None:
        payload = b"" if cookie is None else cookie.to_bytes(4, "little")
        self._request(proto.Type.PING, payload)

    # -- drawing ----------------------------------------------------------

    def clear(self, value: int = 0) -> None:
        """`value`: 0 = all pixels light/off, 1 = all pixels dark/on."""
        self._request(proto.Type.CLEAR, _encode_clear(value))

    def text(
        self,
        x: int,
        y: int,
        text: str,
        font_id: int = 0,
        style: int = 0,
    ) -> None:
        encoded = text.encode("utf-8")
        self._request(proto.Type.DRAW_TEXT, _encode_draw_text(x, y, font_id, style, encoded))

    def draw_rect(self, x: int, y: int, w: int, h: int, op: int = 0, value: int = 1) -> None:
        self._request(proto.Type.DRAW_RECT, _encode_draw_rect(x, y, w, h, op, value))

    def draw_bitmap(self, x: int, y: int, w: int, h: int, bpp: int, packed: bytes, op: int = 0) -> None:
        """One-shot blit for images small enough to fit in a single frame
        (payload <= 4096 - 10 bytes of header). Larger images should use
        `image()`, which streams via FRAME_*."""
        max_pixels_payload = proto.MAX_PAYLOAD - 10
        if len(packed) > max_pixels_payload:
            raise ValueError(
                f"packed bitmap is {len(packed)} bytes, exceeds single-frame budget "
                f"({max_pixels_payload}); use Device.image() instead"
            )
        self._request(proto.Type.DRAW_BITMAP, _encode_draw_bitmap(x, y, w, h, bpp, op, packed))

    def image(
        self,
        image: "str | Image.Image",
        bpp: Optional[int] = None,
        dither: str = "floyd",
        refresh: int = FULL_REFRESH_MODE,
        bg: int = 255,
    ) -> render.Levels:
        """Render `image` (a path or a PIL Image) to fill the whole panel and
        stream it to the device via FRAME_BEGIN/FRAME_DATA/FRAME_END.

        Returns the quantized `Levels` array so a caller (e.g. a dashboard
        or mirror loop) can keep it as the "previous frame" for
        `render.diff_dirty_rect` on the next call.
        """
        pil_image = Image.open(image) if isinstance(image, str) else image
        width, height = self.display_size
        chosen_bpp = bpp if bpp is not None else self.native_bpp
        if chosen_bpp == 2 and not (self.hello_ack.caps & 0x02):
            raise NackReceived(proto.ErrorCode.E_UNSUPPORTED, 1, 0)  # mirrors device-side capability check

        packed, levels = render.render_image(pil_image, width, height, chosen_bpp, method=dither, bg=bg)
        self._send_frame(width, height, chosen_bpp, packed, refresh=refresh)
        return levels

    def _send_frame(
        self,
        w: int,
        h: int,
        bpp: int,
        packed: bytes,
        refresh: int = FULL_REFRESH_MODE,
        origin: Optional[Tuple[int, int]] = None,
        chunk_size: int = DEFAULT_FRAME_CHUNK,
    ) -> None:
        flags = 0
        origin_x = 0
        if origin is not None:
            # v1 quirk (protocol.md §6.3 note): a partial frame's origin_y
            # rides on the PARTIAL_REFRESH sent immediately before it.
            ox, oy = origin
            self._request(proto.Type.PARTIAL_REFRESH, _encode_partial_refresh(ox, oy, w, h))
            flags |= 0x01
            origin_x = ox

        self._request(proto.Type.FRAME_BEGIN, _encode_frame_begin(w, h, bpp, flags, origin_x))

        offset = 0
        n = len(packed)
        while offset < n:
            chunk = packed[offset : offset + chunk_size]
            self.transport.send(proto.Type.FRAME_DATA, 0, _encode_frame_data(offset, chunk))
            offset += len(chunk)

        frame_crc = proto.crc32(packed)
        self._request(proto.Type.FRAME_END, _encode_frame_end(frame_crc, refresh))

    def refresh(
        self,
        rect: Optional[Tuple[int, int, int, int]] = None,
        full: bool = False,
        force_reinit: bool = False,
    ) -> None:
        """Push the back buffer to the panel.

        `full=True` sends FULL_REFRESH (slow; the "unstick a confused panel"
        escape — see docs/protocol.md §6.3). Otherwise `rect=(x, y, w, h)`
        sends PARTIAL_REFRESH for just that region; the whole buffer if
        `rect` is omitted and `full` is False is not a legal single call —
        callers wanting a full push of unchanged-geometry content should
        pass `full=True` or `rect=(0, 0, w, h)`.
        """
        if full:
            self._request(proto.Type.FULL_REFRESH, _encode_full_refresh(1 if force_reinit else 0))
            return
        if rect is None:
            raise ValueError("refresh() needs rect=(x, y, w, h) or full=True")
        x, y, w, h = rect
        self._request(proto.Type.PARTIAL_REFRESH, _encode_partial_refresh(x, y, w, h))

    # -- device control -------------------------------------------------

    def set_backlight(self, level: int, fade_ms: int = 0) -> None:
        self._request(proto.Type.SET_BACKLIGHT, _encode_set_backlight(level, fade_ms))

    def set_contrast(self, value: int) -> None:
        self._request(proto.Type.SET_CONTRAST, _encode_set_contrast(value))

    def set_mode(self, mode: int, persist: bool = False) -> None:
        self._request(proto.Type.SET_MODE, _encode_set_mode(mode, 0x01 if persist else 0x00))

    def battery(self) -> bytes:
        reply = self._request(proto.Type.GET_BATTERY)
        return reply.payload  # raw 8 bytes; decode helper can be added alongside _decode_status

    def reboot(self) -> None:
        self._request(proto.Type.REBOOT, _encode_reboot())

    def boot_original(self) -> None:
        """Switch the boot slot back to the stock image and reboot.

        The device — never the host — selects which slot; see
        docs/protocol.md §6.4. Callers (cli.py) are responsible for any
        user-facing confirmation *before* calling this.
        """
        self._request(proto.Type.BOOT_ORIGINAL, _encode_boot_original())

    def set_time(
        self,
        dt: Optional[datetime] = None,
        *,
        utc: bool = False,
        weekday: Optional[int] = None,
    ) -> datetime:
        """Sets the device's RTC (PCF8563) via SET_TIME (docs/protocol.md
        §6.4, v1.1). Callers (cli.py) are responsible for checking
        `hello_ack.caps & proto.CAP_RTC` first — the device NACKs
        E_UNSUPPORTED anyway if it has no working RTC, but a host UI
        should not offer the control at all in that case (§6.6).

        Default (`dt=None`, `utc=False`): sends the host's current LOCAL
        wall-clock time. This is the right default because the on-device
        CLOCK screen (`byok_clock`) displays whatever the RTC holds with
        no timezone conversion of its own (`byok_rtc.c` stores/returns
        plain year/month/day/hour/minute/second, nothing else) — so
        "local" here means "what the device's face should show", the same
        way you'd set any ordinary clock. Pass `utc=True` to send UTC
        instead (e.g. for a device meant to be read as UTC).

        `dt`: send this datetime instead of "now". If naive (no
        `tzinfo`), its fields are sent exactly as given — `utc` then only
        describes how to interpret it, it does no conversion. If aware,
        it is converted to local or UTC time (per `utc`) before sending.

        `weekday`: 0-6. §6.4: opaque on the wire — the device round-trips
        whatever is sent without assigning it a Sunday-is-0 or
        Monday-is-0 meaning of its own. Defaults to Python's own
        `datetime.weekday()` (Monday=0 .. Sunday=6) — an arbitrary but
        documented choice, since the device does not care which
        convention is used.

        Returns the `datetime` actually sent (useful for a caller that
        wants to report back what was set, e.g. cli.py).
        """
        if dt is None:
            dt = datetime.now(timezone.utc) if utc else datetime.now()
        elif dt.tzinfo is not None:
            dt = dt.astimezone(timezone.utc) if utc else dt.astimezone()

        wd = dt.weekday() if weekday is None else weekday
        payload = _encode_set_time(dt.year, dt.month, dt.day, wd, dt.hour, dt.minute, dt.second)
        self._request(proto.Type.SET_TIME, payload)
        return dt

    # docs/protocol.md §6.4b (v1.2, firmware 0.1.14) -- reconciled: an
    # earlier host-side draft's type codes/payload shapes for these three
    # were corrected against the firmware-side definitions once those
    # landed in docs/protocol.md (see proto.py's Type enum comment and
    # docs/host-tools.md's "Protocol reconciliation" section).

    def get_docstats(self) -> DocStats:
        """`GET_DOCSTATS` -> `DOCSTATS` (docs/protocol.md §6.4b). Manuscript
        stats for `widgets/writing.py`'s daily-goal/word-count widget:
        total file/word/byte counts (device-side: `*.txt` under
        `/SDCARD/Projects`, recursive, capped at 200 files), the newest
        file's mtime (epoch seconds), and words written today (by the
        device's own RTC date). Answered from the device's own
        background-scan snapshot -- this call never blocks on the
        device's own SD I/O. Raises `NackReceived` if the device does not
        implement it (`E_UNKNOWN_TYPE` on any firmware older than 0.1.14)
        -- callers (writing.py's `DeviceDocStatsProvider`) catch that and
        degrade to "--", same as every other provider's failure mode in
        this codebase."""
        reply = self._request(proto.Type.GET_DOCSTATS)
        return _decode_docstats(reply.payload)

    def set_presets(self, names) -> None:
        """`SET_PRESETS` (docs/protocol.md §6.4b) -- sends the dashboard's
        preset manifest (ordered display names, `byok.dashboard.presets`)
        to the device once at connect, so its own preset menu can name
        and select one. `names`: an ordered iterable of at most
        `SET_PRESETS_MAX_COUNT` (8) display-name strings, each NUL-padded
        to a fixed 20 bytes on the wire (`_encode_set_presets`) --
        `byok.dashboard.presets.MAX_DISPLAY_NAME_LEN` enforces the same
        20-byte cap at manifest-validation time, so a well-formed
        manifest's names are never actually truncated here. Raises
        `NackReceived` if the device does not implement it, or
        `ValueError` (from `_encode_set_presets`) if `names` has more
        than 8 entries; callers should treat a `NackReceived` as "no
        device-side menu available" and continue running the host-
        selected config (see `dashboard/loop.py`'s handling)."""
        self._request(proto.Type.SET_PRESETS, _encode_set_presets(names))

    def display_cfg_bulk(self, on: bool) -> None:
        """`DISPLAY_CFG` (docs/protocol.md §6.4b) -- toggles the device's
        bulk-vs-per-byte I2C write posture for panel data (`byok display
        --bulk on|off`, cli.py; `docs/design-rationale.md` D-021, `docs/sample-projects/doom.md`
        §6's bulk-write speed test). Takes effect on the panel's next
        refresh; the device logs the measured refresh duration either
        way (device-side log only -- no wire field carries it back)."""
        self._request(proto.Type.DISPLAY_CFG, _encode_display_cfg(on))
