#!/usr/bin/env python3
"""byok_panel.py — standalone contrast/backlight/status probe for the BYOK panel.

A small, independent tool (not part of `byok.cli`) for exercising three
messages from docs/protocol.md while 0.1.4's display-quality work is
in progress: `SET_CONTRAST` (0x22), `SET_BACKLIGHT` (0x21), `GET_STATUS`
(0x05). It talks the wire directly through the existing package modules —
`byok.proto` for frame encode/decode and `byok.transport` for product-string
discovery, connect, and request/reply — rather than duplicating any of that
logic. It does not import `byok.device` or `byok.cli`, and it does not touch
firmware/ or host/macos/byok/cli.py.

Usage:
    host/macos/.venv/bin/python host/tools/byok_panel.py contrast VALUE [--dry-run]
    host/macos/.venv/bin/python host/tools/byok_panel.py backlight VALUE [--fade MS] [--dry-run]
    host/macos/.venv/bin/python host/tools/byok_panel.py status [--dry-run]
    host/macos/.venv/bin/python host/tools/byok_panel.py sweep [--dry-run]

    --port DEVICE     use this serial device instead of auto-discovery
                       (still requires a matching product string)
    --dry-run         build and print the exact frame that would be sent —
                       hex bytes only, no serial port is opened, no device
                       is touched

Value ranges (see docs/protocol.md Sec.6.4 and firmware/s3/components/
byok_display/byok_display.c):

  contrast VALUE   0-255, the raw wire byte SET_CONTRAST carries. The
                   firmware re-scales it internally: pct = VALUE * 99 / 255
                   (the vendor's UC1611 driver works in a 0-99 "percent"
                   domain, hw_config.h BYOK_LCD_CONTRAST_PCT_MAX), then
                   reg = pct * 255 / 99 is written as the data byte
                   following controller command 0x81
                   (BYOK_LCD_CMD_SET_CONTRAST). So VALUE is NOT the percent
                   and is NOT the raw register value — it's the wire byte,
                   two rescale steps removed from the register. The stock
                   default register is 0x50 (80 decimal): pct = 80*99/255
                   ~= 31%, i.e. the panel ships around 31% contrast.

  backlight VALUE  0-255 (0 = off), the raw wire byte SET_BACKLIGHT carries.
                   Firmware: pct = VALUE * 100 / 255, duty = pct *
                   BYOK_BACKLIGHT_DUTY_MAX / 100 (13-bit LEDC, so duty max
                   8191). --fade MS is the wire's own fade_ms field, u16,
                   0-5000ms, 0 = immediate.

Discovery is identical to `byok.cli`: only a serial port whose USB product
string contains "BYOK Mod Display" is ever opened (byok.transport's own
invariant) — the stock firmware's ports are never matched.
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host" / "macos"))

from byok import proto  # noqa: E402
from byok import transport  # noqa: E402

# -- payload encoders, matching docs/protocol.md Sec.6.4 / Sec.6.1 exactly
# (same struct formats as host/macos/byok/device.py's _enc_set_backlight /
# _enc_set_contrast, verified against that module rather than re-derived). --

_SET_BACKLIGHT_STRUCT = "<BH"   # level u8, fade_ms u16
_SET_CONTRAST_STRUCT = "<B"    # value u8
_STATUS_STRUCT_A = "<HBBBBBB"  # battery_mv, battery_pct, charging, mode, backlight, contrast, flags
_STATUS_STRUCT_B = "<III"      # uptime_ms, free_heap, min_free_heap
_NACK_STRUCT = "<BBH"          # code, detail, seq_echo

SWEEP_VALUES = list(range(10, 91, 10))  # 10, 20, ..., 90
SWEEP_PAUSE_S = 1.5


class RangeError(ValueError):
    """A parsed argument was an int but out of its legal wire range."""


def _u8(name: str, value: int) -> int:
    if not (0 <= value <= 255):
        raise RangeError(f"{name} must be 0-255, got {value}")
    return value


def encode_contrast(value: int) -> bytes:
    return struct.pack(_SET_CONTRAST_STRUCT, value)


def encode_backlight(level: int, fade_ms: int) -> bytes:
    return struct.pack(_SET_BACKLIGHT_STRUCT, level, fade_ms)


def contrast_pct(value: int) -> float:
    """What the firmware's internal 0-99 percent domain works out to for a
    given wire VALUE — informational only, matches byok_display.c's own
    integer math (pct = value * 99 // 255)."""
    return (value * 99) // 255


def build_frame(msg_type: int, payload: bytes, seq: int = 0) -> bytes:
    """The exact frame a fresh SerialTransport.request() would put on the
    wire for this message: ACK_REQ set, SEQ as given (a brand-new
    SerialTransport's counter starts at 0 — see transport.py's __init__)."""
    return proto.encode_frame(msg_type, int(proto.Flags.ACK_REQ), seq, payload)


def print_frame(label: str, msg_type: int, payload: bytes) -> None:
    frame = build_frame(msg_type, payload)
    print(f"{label}: {frame.hex()}  ({len(frame)} bytes)")


def _error_name(code: int) -> str:
    try:
        return proto.ErrorCode(code).name
    except ValueError:
        return f"0x{code:02X}"


def _print_reply(reply: "proto.Frame") -> None:
    if reply.type == proto.Type.ACK:
        print("  -> ACK")
    elif reply.type == proto.Type.NACK:
        code, detail, seq_echo = struct.unpack_from(_NACK_STRUCT, reply.payload, 0)
        print(f"  -> NACK code={_error_name(code)} detail={detail} seq_echo={seq_echo}")
    else:
        print(f"  -> unexpected reply type 0x{reply.type:02X}: {reply.payload.hex()}")


def _print_status_payload(payload: bytes) -> None:
    if len(payload) != 20:
        print(f"  STATUS payload is {len(payload)} bytes, expected 20: {payload.hex()}")
        return
    battery_mv, battery_pct, charging, mode, backlight, contrast, flags = (
        struct.unpack_from(_STATUS_STRUCT_A, payload, 0)
    )
    uptime_ms, free_heap, min_free_heap = struct.unpack_from(_STATUS_STRUCT_B, payload, 8)
    print(f"  battery       {battery_mv} mV ({battery_pct}%)")
    print(f"  charging      {charging}")
    print(f"  mode          {mode}")
    print(f"  backlight     {backlight}")
    print(f"  contrast      {contrast}")
    print(f"  flags         0x{flags:02X}")
    print(f"  uptime        {uptime_ms} ms")
    print(f"  free heap     {free_heap} bytes (min {min_free_heap})")


def _connect(args: argparse.Namespace) -> transport.SerialTransport:
    t = transport.SerialTransport(device=args.port)
    try:
        t.connect(do_hello=True)
    except transport.NoDeviceFound as exc:
        print(f"error: {exc}", file=sys.stderr)
        print(
            "hint: is the device plugged in and running our firmware? "
            "the stock firmware's ports are never matched — see byok/transport.py.",
            file=sys.stderr,
        )
        raise SystemExit(2) from exc
    except transport.TransportError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc
    return t


def cmd_contrast(args: argparse.Namespace) -> int:
    payload = encode_contrast(args.value)
    if args.dry_run:
        print_frame(f"contrast {args.value} (SET_CONTRAST, pct~={contrast_pct(args.value)})",
                    proto.Type.SET_CONTRAST, payload)
        return 0
    t = _connect(args)
    try:
        print(f"contrast {args.value} (pct~={contrast_pct(args.value)})")
        reply = t.request(proto.Type.SET_CONTRAST, payload)
        _print_reply(reply)
    finally:
        t.close()
    return 0


def cmd_backlight(args: argparse.Namespace) -> int:
    payload = encode_backlight(args.value, args.fade)
    if args.dry_run:
        print_frame(f"backlight {args.value} fade_ms={args.fade} (SET_BACKLIGHT)",
                    proto.Type.SET_BACKLIGHT, payload)
        return 0
    t = _connect(args)
    try:
        print(f"backlight {args.value} fade_ms={args.fade}")
        reply = t.request(proto.Type.SET_BACKLIGHT, payload)
        _print_reply(reply)
    finally:
        t.close()
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    if args.dry_run:
        print_frame("status (GET_STATUS)", proto.Type.GET_STATUS, b"")
        return 0
    t = _connect(args)
    try:
        reply = t.request(proto.Type.GET_STATUS, b"")
        if reply.type == proto.Type.STATUS:
            print("status:")
            _print_status_payload(reply.payload)
        elif reply.type == proto.Type.NACK:
            code, detail, seq_echo = struct.unpack_from(_NACK_STRUCT, reply.payload, 0)
            print(f"NACK code={_error_name(code)} detail={detail} seq_echo={seq_echo}")
        else:
            print(f"unexpected reply type 0x{reply.type:02X}: {reply.payload.hex()}")
    finally:
        t.close()
    return 0


def cmd_sweep(args: argparse.Namespace) -> int:
    if args.dry_run:
        for value in SWEEP_VALUES:
            print_frame(f"contrast {value} (pct~={contrast_pct(value)})",
                        proto.Type.SET_CONTRAST, encode_contrast(value))
        return 0
    t = _connect(args)
    try:
        print(f"sweeping contrast {SWEEP_VALUES[0]}..{SWEEP_VALUES[-1]} "
              f"step 10, {SWEEP_PAUSE_S}s pause — watch the panel")
        for value in SWEEP_VALUES:
            print(f"contrast {value} (pct~={contrast_pct(value)})")
            reply = t.request(proto.Type.SET_CONTRAST, encode_contrast(value))
            _print_reply(reply)
            time.sleep(SWEEP_PAUSE_S)
    finally:
        t.close()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="byok_panel.py",
        description="Standalone SET_CONTRAST / SET_BACKLIGHT / GET_STATUS probe (not byok.cli).",
    )
    parser.add_argument(
        "--port", default=None,
        help="serial device with a matching product string (default: auto-discover)",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="print the frame(s) that would be sent as hex; never opens a port",
    )

    sub = parser.add_subparsers(dest="command", required=True)

    p_contrast = sub.add_parser("contrast", help="send SET_CONTRAST")
    p_contrast.add_argument("value", type=int, help="0-255, wire value (see module docstring)")
    p_contrast.set_defaults(func=cmd_contrast)

    p_backlight = sub.add_parser("backlight", help="send SET_BACKLIGHT")
    p_backlight.add_argument("value", type=int, help="0-255, 0=off")
    p_backlight.add_argument("--fade", type=int, default=0, help="fade_ms, 0-5000 (default 0 = immediate)")
    p_backlight.set_defaults(func=cmd_backlight)

    p_status = sub.add_parser("status", help="send GET_STATUS and print the reply")
    p_status.set_defaults(func=cmd_status)

    p_sweep = sub.add_parser(
        "sweep",
        help=f"sweep contrast {SWEEP_VALUES[0]}..{SWEEP_VALUES[-1]} step 10, "
             f"{SWEEP_PAUSE_S}s pause each, printing the value",
    )
    p_sweep.set_defaults(func=cmd_sweep)

    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    # Validate u8 ranges post-parse so argparse's own error formatting for
    # "not an int at all" still fires first.
    try:
        if args.command == "contrast":
            _u8("value", args.value)
        elif args.command == "backlight":
            _u8("value", args.value)
            if not (0 <= args.fade <= 5000):
                raise RangeError("--fade must be 0-5000")
    except RangeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    try:
        return args.func(args)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    except transport.LinkDead as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
