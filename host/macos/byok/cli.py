"""`byok` command-line entry point.

byok info | status | clear | text [X Y] TEXT | image PATH | dashboard | mirror
     | original | sleep | settime [--utc] [ISO_DATETIME] | notify TEXT [--seconds N]
     | display --bulk on|off
"""

from __future__ import annotations

import argparse
import logging
import sys
import time
from datetime import datetime
from typing import Optional, Sequence

from . import notify_ipc, proto, render
from .device import Device, NackReceived
from .transport import HandshakeFailed, NoDeviceFound, PortBusy, TransportError, WrongDevice

logger = logging.getLogger("byok.cli")


def _connect(args: argparse.Namespace) -> Device:
    unsafe_port = getattr(args, "unsafe_port", None)
    port = unsafe_port or args.port
    allow_unmatched = bool(unsafe_port)
    if unsafe_port:
        # --unsafe-port bypasses the product-string gate entirely, so make
        # the operator confirm the exact node before we open it.
        answer = input(
            f"--unsafe-port will open {unsafe_port!r} WITHOUT checking that it is our "
            "device (it may be the stock firmware's port, or an unrelated device, and "
            "opening it can reset whatever is on the other end). Type the device path "
            "again to confirm: "
        )
        if answer.strip() != unsafe_port:
            print("confirmation did not match — aborting", file=sys.stderr)
            raise SystemExit(2)
    try:
        return Device.open(port=port, allow_unmatched_port=allow_unmatched)
    except WrongDevice as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc
    except NoDeviceFound as exc:
        print(f"error: {exc}", file=sys.stderr)
        print(
            "hint: is the device plugged in and running our firmware? "
            "the stock firmware's ports are never matched — see transport.py.",
            file=sys.stderr,
        )
        raise SystemExit(2) from exc
    except HandshakeFailed as exc:
        print(f"error: HELLO handshake failed: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc
    except TransportError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc


def cmd_info(args: argparse.Namespace) -> int:
    with _connect(args) as dev:
        info = dev.info()
        h = dev.hello_ack
        print(f"firmware      {info.fw_version}")
        print(f"idf           {info.idf_version}")
        print(f"built         {info.build_date}")
        print(f"flash         {info.flash_size} bytes")
        print(f"psram         {info.psram_size} bytes")
        print(f"cpu           {info.cpu_hz} Hz")
        print(f"display       {h.display_w}x{h.display_h} ({h.display_bpp_native} bpp native)")
        print(f"boot slot     {h.boot_slot}")
        print(f"device id     {h.device_id.hex()}")
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    with _connect(args) as dev:
        s = dev.status()
        print(f"battery       {s.battery_mv} mV ({s.battery_pct}%)")
        print(f"charging      {s.charging}")
        print(f"mode          {s.mode}")
        print(f"backlight     {s.backlight}")
        print(f"contrast      {s.contrast}")
        print(f"uptime        {s.uptime_ms} ms")
        print(f"free heap     {s.free_heap} bytes (min {s.min_free_heap})")
        print(f"flags         0x{s.flags:02X}")
    return 0


def cmd_clear(args: argparse.Namespace) -> int:
    with _connect(args) as dev:
        dev.clear(value=args.value)
        if not args.no_refresh:
            w, h = dev.display_size
            dev.refresh(rect=(0, 0, w, h))
    return 0


def cmd_text(args: argparse.Namespace) -> int:
    # `text_args` is 1 or 3 positional strings (see build_parser()'s "text"
    # subparser): "TEXT" alone defaults x/y to 0 0; "X Y TEXT" is unchanged
    # from before this convenience existed. Parsed here, not by argparse
    # itself, because argparse has no clean way to make a *leading* pair of
    # positionals optional while a trailing one stays required.
    if len(args.text_args) == 1:
        x, y, text = 0, 0, args.text_args[0]
    elif len(args.text_args) == 3:
        try:
            x, y = int(args.text_args[0]), int(args.text_args[1])
        except ValueError:
            print("byok text: X and Y must be integers", file=sys.stderr)
            return 2
        text = args.text_args[2]
    else:
        print("byok text: expected 'TEXT' or 'X Y TEXT'", file=sys.stderr)
        return 2

    with _connect(args) as dev:
        style = (0x01 if args.invert else 0) | (0x02 if args.wrap else 0)
        dev.text(x, y, text, font_id=args.font, style=style)
        if not args.no_refresh:
            w, h = dev.display_size
            dev.refresh(rect=(0, 0, w, h))
    return 0


def cmd_image(args: argparse.Namespace) -> int:
    with _connect(args) as dev:
        dev.image(args.path, bpp=args.bpp, dither=args.dither)
    return 0


def cmd_dashboard(args: argparse.Namespace) -> int:
    from .dashboard import config as dash_config
    from .dashboard import media_transport
    from .dashboard import presets as presets_mod
    from .dashboard.fonts import FontSet
    from .dashboard.loop import DashboardLoop
    from .dashboard.preview import _live_providers
    from .dashboard.widgets import writing as writing_widget

    if args.list_examples:
        names = dash_config.list_examples()
        if not names:
            print(f"no example configs found in {dash_config.examples_dir()}")
            return 0
        print(f"example configs in {dash_config.examples_dir()}:")
        for name in names:
            print(f"  {name}")
        print("\nuse with: byok dashboard --config <name>  (bare name, name.yaml, or a full path all work)")
        return 0

    if args.list_presets:
        try:
            entries = presets_mod.default_presets()
        except (presets_mod.PresetError, OSError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        print("bundled presets (order = device menu / SET_PRESETS order):")
        for entry in entries:
            print(f"  {entry.name:<10} {entry.display}")
        print("\nuse with: byok dashboard --preset <name>")
        return 0

    if args.config and args.preset:
        print("error: --config and --preset are mutually exclusive", file=sys.stderr)
        return 2

    # Three modes, decided once here and used for the rest of this
    # function -- see docs/host-tools.md's "Presets" section for the full
    # rationale:
    #
    #  1. --config PATH (unchanged from before presets existed): exactly that
    #     one config, forever -- no SET_PRESETS is sent, no live
    #     PRESET_CHANGED/menu switching. `preset_entries` stays None.
    #  2. --preset NAME: starts on that preset, SET_PRESETS is still sent
    #     (so the device's own menu is populated and legible), but this
    #     host then ignores PRESET_CHANGED -- `preset_entries` is set and
    #     `preset_switching_locked` gets set True right after force_preset()'s
    #     own initial selection call.
    #  3. neither flag (the new default): starts on the manifest's first
    #     entry, SET_PRESETS is sent, and PRESET_CHANGED freely switches
    #     the live config -- the normal "device menu drives the panel"
    #     mode the SET_PRESETS/PRESET_CHANGED design is for.
    preset_entries = None
    forced_preset_index = None
    lock_after_forcing = False

    if args.preset:
        try:
            preset_entries = presets_mod.default_presets()
            forced = presets_mod.find_preset(args.preset, preset_entries)
        except presets_mod.PresetError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        cfg = presets_mod.load_preset_config(forced)
        forced_preset_index = preset_entries.index(forced)
        lock_after_forcing = True
    elif args.config:
        # Resolves relative to the cwd first (an explicit ./foo.yaml or
        # /abs/path.yaml keeps working exactly as before), then falls
        # back to the bundled examples/ dir -- see resolve_config_path()'s
        # own docstring for the full three-step search order.
        try:
            config_path = dash_config.resolve_config_path(args.config)
        except FileNotFoundError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        try:
            cfg = dash_config.load(config_path)
        except dash_config.ConfigError as exc:
            print(f"error: invalid dashboard config {config_path!r}: {exc}", file=sys.stderr)
            return 2
        except OSError as exc:
            print(f"error: could not read dashboard config {config_path!r}: {exc}", file=sys.stderr)
            return 2
    else:
        try:
            preset_entries = presets_mod.default_presets()
        except (presets_mod.PresetError, OSError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        cfg = presets_mod.load_preset_config(preset_entries[0])
        forced_preset_index = 0

    with _connect(args) as dev:
        providers = _live_providers()
        # writing.py's only real provider needs a connected Device -- see
        # preview.py's _live_providers() docstring for why it isn't
        # included there.
        providers["writing"] = writing_widget.DeviceDocStatsProvider(dev)

        loop = DashboardLoop(
            dev,
            cfg,
            interval=args.interval,
            full_every=args.full_every,
            preview_path=args.preview,
            providers=providers,
            fonts=FontSet(ttf_path=cfg.fonts.get("path")),
            presets=preset_entries,
        )

        if preset_entries is not None:
            if forced_preset_index is not None:
                loop.force_preset(forced_preset_index)
            if lock_after_forcing:
                loop.preset_switching_locked = True
            try:
                dev.set_presets(presets_mod.display_names(preset_entries))
            except Exception as exc:  # noqa: BLE001 - firmware older than
                # 0.1.14 (docs/protocol.md §6.4b) NACKs E_UNKNOWN_TYPE, or
                # the link times out -- either way this host keeps running
                # with its own chosen starting preset, just without the
                # device's own menu in sync.
                logger.warning(
                    "SET_PRESETS not accepted by this device (%s) -- continuing with "
                    "the chosen preset selected locally; the device's own menu (if it "
                    "has one) may not reflect this", exc,
                )
            # Wired regardless of locked/free-switching mode -- see
            # DashboardLoop.presets' own comment: active_preset_name keeps
            # reporting correctly either way, so the media-only gate below
            # is unaffected by --preset's lock.
            loop.on_button = media_transport.make_button_handler(
                lambda: loop.active_preset_name == "media"
            )

        loop.run(once=args.once)
    return 0


def cmd_mirror(args: argparse.Namespace) -> int:
    print("mirror: not yet implemented (M6 — see docs/host-tools.md)")
    return 1


def cmd_sleep(args: argparse.Namespace) -> int:
    print("sleep: not yet implemented")
    return 1


def cmd_original(args: argparse.Namespace) -> int:
    if not args.yes:
        print(
            "This reboots the device into the STOCK firmware (BOOT_ORIGINAL). "
            "Our firmware will not run again until you re-flash it or the SD "
            "updater path is used again."
        )
        try:
            answer = input("Type 'yes' to continue: ")
        except EOFError:
            answer = ""
        if answer.strip().lower() != "yes":
            print("aborted")
            return 1

    with _connect(args) as dev:
        h = dev.hello_ack
        if not (h.caps & 0x100):
            print(
                "error: device reports no stock image available "
                "(HELLO_ACK caps bit 8 clear) — refusing to send BOOT_ORIGINAL",
                file=sys.stderr,
            )
            return 2
        try:
            dev.boot_original()
        except NackReceived as exc:
            print(f"error: device refused: {exc}", file=sys.stderr)
            return 2
        print("device is rebooting into stock firmware")
    return 0


def cmd_settime(args: argparse.Namespace) -> int:
    dt = None
    if args.datetime:
        try:
            dt = datetime.fromisoformat(args.datetime)
        except ValueError:
            print(
                f"byok settime: could not parse {args.datetime!r} as an ISO 8601 "
                "datetime (e.g. 2026-09-03T14:30:00)",
                file=sys.stderr,
            )
            return 2

    with _connect(args) as dev:
        h = dev.hello_ack
        if not (h.caps & proto.CAP_RTC):
            print(
                "error: device reports no RTC (HELLO_ACK caps bit 12 clear) — "
                "refusing to send SET_TIME",
                file=sys.stderr,
            )
            return 2
        try:
            sent = dev.set_time(dt, utc=args.utc)
        except NackReceived as exc:
            print(f"error: device refused: {exc}", file=sys.stderr)
            return 2
        label = "UTC" if args.utc else "local"
        print(f"device RTC set to {sent.isoformat(timespec='seconds')} ({label})")
    return 0


# docs/protocol.md §6.2 DRAW_TEXT style bit0 -- inverted glyphs, i.e. light
# text against whatever's already in the back buffer (here, the filled-dark
# banner rect drawn just before it).
_DRAW_TEXT_STYLE_INVERTED = 0x01
_NOTIFY_BANNER_HEIGHT_PX = 16


def _draw_banner_direct(dev: Device, text: str, height: int = _NOTIFY_BANNER_HEIGHT_PX) -> "tuple[int, int, int, int]":
    """Draws an inverted banner across the top rows via literal
    DRAW_RECT(filled)/DRAW_TEXT(inverted style)/PARTIAL_REFRESH device
    commands -- the standalone (no dashboard loop running) `byok notify`
    path. See `byok/notify_ipc.py`'s module docstring for why the
    loop-mediated path (a loop already owns the port) instead composites
    the banner into its own rendered frame rather than reusing this
    function. Returns the `(x, y, w, h)` rect drawn, for `_clear_banner_direct`."""
    w, h = dev.display_size
    bh = min(height, max(1, h // 3))
    dev.draw_rect(0, 0, w, bh, op=1, value=1)  # op 1 = filled, value 1 = dark
    dev.text(2, 2, text, style=_DRAW_TEXT_STYLE_INVERTED)
    dev.refresh(rect=(0, 0, w, bh))
    return (0, 0, w, bh)


def _clear_banner_direct(dev: Device, rect: "tuple[int, int, int, int]") -> None:
    """Restores the banner's rect to background -- op 2 clear region
    (docs/protocol.md §6.2's DRAW_RECT table) + PARTIAL_REFRESH. The
    standalone path has no memory of whatever was on screen before the
    banner (no render pipeline is involved at all here, unlike the
    loop-mediated path) -- clearing to background is the documented,
    honest limit of what "restore" can mean without one. See
    `docs/host-tools.md`'s "Notify" section."""
    x, y, w, h = rect
    dev.draw_rect(x, y, w, h, op=3, value=0)  # op 3 = clear region
    dev.refresh(rect=rect)


def cmd_notify(args: argparse.Namespace) -> int:
    text = args.text
    seconds = args.seconds

    # IPC-first: if a dashboard/mirror loop's lock says one is alive
    # (byok.notify_ipc.write_lock, written by DashboardLoop.run() for the
    # duration of its run), go straight to the request file that loop
    # polls once per render cycle (dashboard/loop.py's
    # `_apply_notify_overlay`) -- never attempt Device.open() at all.
    # Before this, `cmd_notify` tried the direct-open path first no
    # matter what; since the port previously opened non-exclusively
    # (transport.py, fixed 2026-09-04), that second open could succeed,
    # re-HELLO the device, and wipe the running loop's SEQ session out
    # from under it (see transport.PortBusy's docstring for the full
    # incident writeup) -- checking the lock first avoids ever attempting
    # that second open in the common case where a loop is in fact running.
    if notify_ipc.loop_is_running():
        notify_ipc.write_request(text, seconds=seconds)
        print(
            f"byok notify: a running 'byok dashboard'/'byok mirror' loop owns the "
            f"device -- queued for it instead of opening the port directly "
            f"(byok.notify_ipc, {notify_ipc.DEFAULT_NOTIFY_PATH})."
        )
        return 0

    try:
        dev = Device.open(port=args.port)
    except PortBusy as exc:
        # The loop lock said nothing was running (missing, or a stale
        # lock from a crashed process), but the port is genuinely held --
        # some other process (a loop that hasn't written its lock yet, a
        # manual `byok` invocation, or a process this project doesn't
        # know about) has it open. Same IPC fallback as the lock-hit case
        # above: never attempt a second, port-sharing open now that the
        # factory opens exclusively (transport.py) -- that open would
        # simply fail again, loudly, every time.
        notify_ipc.write_request(text, seconds=seconds)
        print(
            f"byok notify: device port is busy ({exc}) -- queued for the process "
            f"holding it instead (byok.notify_ipc, {notify_ipc.DEFAULT_NOTIFY_PATH}). "
            f"If that process is not a 'byok dashboard'/'byok mirror' loop, this "
            f"banner will never be drawn."
        )
        return 0
    except (WrongDevice, NoDeviceFound, HandshakeFailed, TransportError, OSError) as exc:
        # No loop running and no device reachable at all (unplugged,
        # wrong firmware, handshake failure, ...) -- queue anyway: it is
        # harmless (a loop that starts later still picks it up within its
        # `expires_at`) and matches this command's existing degrade-
        # gracefully posture for every other failure mode.
        req = notify_ipc.write_request(text, seconds=seconds)
        print(
            f"byok notify: could not open the device directly ({exc}) -- queued for a "
            f"running 'byok dashboard' loop instead (byok.notify_ipc, "
            f"{notify_ipc.DEFAULT_NOTIFY_PATH}). If no dashboard loop is currently "
            f"running, this banner will never be drawn -- start one, or plug in the "
            f"device with nothing else connected to it, and try again."
        )
        return 0

    try:
        rect = _draw_banner_direct(dev, text)
        try:
            time.sleep(max(0.0, seconds))
        except KeyboardInterrupt:
            pass
        _clear_banner_direct(dev, rect)
    finally:
        dev.close()
    return 0


# docs/protocol.md §6.4b's DISPLAY_CFG has no capability bit of its own
# (§6.6's bitmap table is unchanged by v1.2) -- byok display doesn't gate
# on one the way `original`/`settime` do, since none is defined for it.
# A device that doesn't implement it (older than 0.1.14) NACKs
# E_UNKNOWN_TYPE, caught below same as everywhere else in this file.
def cmd_display(args: argparse.Namespace) -> int:
    on = args.bulk == "on"
    with _connect(args) as dev:
        try:
            dev.display_cfg_bulk(on)
        except NackReceived as exc:
            print(f"error: device refused: {exc}", file=sys.stderr)
            return 2
    print(f"display bulk mode: {'on' if on else 'off'}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="byok", description="BYOK Mod host tool")
    parser.add_argument("--port", default=None, help="serial device with a matching product string, e.g. /dev/cu.usbmodemXXXX (default: auto-discover by product string). Refused if the port's product string does not match ours.")
    parser.add_argument(
        "--unsafe-port",
        default=None,
        metavar="DEVICE",
        help="open DEVICE without checking its product string at all (interactive confirmation required). "
        "Only for recovery/debugging; never use this to talk to a device you have not identified yourself.",
    )
    parser.add_argument("--verbose", "-v", action="count", default=0, help="-v for INFO, -vv for DEBUG")

    sub = parser.add_subparsers(dest="command", required=True)

    p_info = sub.add_parser("info", help="print firmware/hardware info")
    p_info.set_defaults(func=cmd_info)

    p_status = sub.add_parser("status", help="print live device status")
    p_status.set_defaults(func=cmd_status)

    p_clear = sub.add_parser("clear", help="clear the back buffer and refresh")
    p_clear.add_argument("--value", type=int, choices=(0, 1), default=0, help="0=light, 1=dark")
    p_clear.add_argument("--no-refresh", action="store_true", help="clear the buffer but don't push it to the panel")
    p_clear.set_defaults(func=cmd_clear)

    p_text = sub.add_parser("text", help="draw a line of text and refresh")
    p_text.add_argument(
        "text_args",
        nargs="+",
        metavar="[X Y] TEXT",
        help="TEXT alone (x/y default to 0 0), or X Y TEXT",
    )
    p_text.add_argument("--font", type=int, default=0, dest="font")
    p_text.add_argument("--invert", action="store_true")
    p_text.add_argument("--wrap", action="store_true")
    p_text.add_argument("--no-refresh", action="store_true")
    p_text.set_defaults(func=cmd_text)

    p_image = sub.add_parser("image", help="render an image file to fill the panel")
    p_image.add_argument("path", type=str)
    p_image.add_argument("--bpp", type=int, choices=(1, 2), default=None, help="default: device's native bpp")
    p_image.add_argument("--dither", choices=sorted(render._QUANTIZERS), default="floyd")
    p_image.set_defaults(func=cmd_image)

    p_dash = sub.add_parser("dashboard", help="render a live status dashboard to the panel")
    p_dash.add_argument(
        "--config", default=None, metavar="PATH",
        help="dashboard YAML config: a path (relative to cwd, or absolute), or the bare name "
        "of one of the bundled examples/ configs (see --list-examples). Mutually exclusive "
        "with --preset. Default (neither flag given): the device-switchable presets set "
        "(see --list-presets), starting on the first one.",
    )
    p_dash.add_argument(
        "--list-examples", action="store_true",
        help="list the bundled example configs (examples/*.yaml) and exit -- no device needed",
    )
    p_dash.add_argument(
        "--preset", default=None, metavar="NAME",
        help="force one bundled preset (see --list-presets) and pin the host to it -- "
        "SET_PRESETS is still sent so the device's own menu is populated, but this host "
        "then ignores PRESET_CHANGED. Mutually exclusive with --config.",
    )
    p_dash.add_argument(
        "--list-presets", action="store_true",
        help="list the bundled device-switchable presets (presets/*.yaml) and exit -- "
        "no device needed",
    )
    p_dash.add_argument(
        "--interval", type=float, default=None, metavar="S",
        help="seconds between frames (default: the config's refresh_seconds)",
    )
    p_dash.add_argument(
        "--full-every", type=int, default=60, metavar="N",
        help="force a full refresh every N frames, 0 to disable (default: 60)",
    )
    p_dash.add_argument("--once", action="store_true", help="render and send a single frame, then exit")
    p_dash.add_argument(
        "--preview", default=None, metavar="PATH",
        help="also write a PNG of each rendered frame to PATH",
    )
    p_dash.set_defaults(func=cmd_dashboard)

    p_mirror = sub.add_parser("mirror", help="(stub) mirror a Mac display/window")
    p_mirror.set_defaults(func=cmd_mirror)

    p_sleep = sub.add_parser("sleep", help="(stub) put the panel to sleep")
    p_sleep.set_defaults(func=cmd_sleep)

    p_original = sub.add_parser("original", help="reboot into the stock firmware (BOOT_ORIGINAL)")
    p_original.add_argument("--yes", action="store_true", help="skip the confirmation prompt")
    p_original.set_defaults(func=cmd_original)

    p_settime = sub.add_parser("settime", help="set the device's RTC (SET_TIME, v1.1)")
    p_settime.add_argument(
        "datetime", nargs="?", default=None, metavar="ISO_DATETIME",
        help="ISO 8601 datetime to set (e.g. 2026-09-03T14:30:00); default: now",
    )
    p_settime.add_argument(
        "--utc", action="store_true",
        help="send UTC instead of local time (default: local, matching what the "
        "on-device CLOCK screen displays with no timezone conversion of its own)",
    )
    p_settime.set_defaults(func=cmd_settime)

    p_notify = sub.add_parser(
        "notify", help="flash a short inverted banner across the top of the panel"
    )
    p_notify.add_argument("text", metavar="TEXT", help="the banner text")
    p_notify.add_argument(
        "--seconds", type=float, default=notify_ipc.DEFAULT_SECONDS, metavar="N",
        help=f"how long the banner stays up before restoring (default: {notify_ipc.DEFAULT_SECONDS:g})",
    )
    p_notify.set_defaults(func=cmd_notify)

    p_display = sub.add_parser("display", help="device display configuration (DISPLAY_CFG)")
    p_display.add_argument(
        "--bulk", choices=("on", "off"), required=True,
        help="toggle the device's bulk display-update mode",
    )
    p_display.set_defaults(func=cmd_display)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    level = logging.WARNING
    if args.verbose == 1:
        level = logging.INFO
    elif args.verbose >= 2:
        level = logging.DEBUG
    logging.basicConfig(level=level, format="%(levelname)s %(name)s: %(message)s")

    try:
        return args.func(args)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
