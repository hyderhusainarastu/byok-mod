"""Media transport control via the device's physical buttons -- active
only while the dashboard loop's current preset is "media"
(`byok.dashboard.presets`, `presets/media.yaml`).

Wires `EVT_BUTTON` (docs/protocol.md §6.5, already fully specified --
not one of the host-side PROPOSED messages) to Music.app / Spotify.app via
`osascript`: UP = next track, DOWN = previous track, BRIGHTNESS =
play/pause. EXECUTE and WAKE are left alone here -- EXECUTE already has
its own device-side meaning (docs/protocol.md §6.4's back-button
paragraph: backlight cycling) once a host is disconnected, and neither it
nor WAKE has an obvious media action.

Only a clean `PRESSED` edge acts -- `RELEASED`/`AUTO_REPEAT`/`LONG_PRESS`
are ignored, so holding UP down doesn't fire a burst of track-skips (no
button in this table currently emits `AUTO_REPEAT` per docs/protocol.md
§6.5's device-side description, but the host-side filter costs nothing
and doesn't assume that stays true).

No-op, silently, if neither Music nor Spotify is running: the single
`osascript` call in `send_media_command` checks both from inside one
AppleScript `if`/`else if`, so "neither app open" produces no error and
takes no action -- this project's usual "degrade to nothing rather than
raise or show an error" convention (docs/host-tools.md §12), applied to a
button handler instead of a widget's `render()`.
"""

from __future__ import annotations

import logging
import subprocess
from typing import Callable, Optional

from .. import device as device_mod

logger = logging.getLogger("byok.dashboard.media_transport")

_TIMEOUT_S = 3.0

_ACTION_BY_BUTTON = {
    device_mod.BUTTON_UP: "next track",
    device_mod.BUTTON_DOWN: "previous track",
    device_mod.BUTTON_BRIGHTNESS: "playpause",
}


def send_media_command(action: str, timeout: float = _TIMEOUT_S) -> None:
    """`action`: an AppleScript command verb valid for both Music.app and
    Spotify.app's dictionaries -- "next track", "previous track",
    "playpause" are the three this module ever sends. Tries Music first,
    then Spotify, whichever is actually running; a no-op if neither is
    (see module docstring). Never raises -- a missing `osascript` binary
    or a slow/hung AppleScript event (bounded by `timeout`) is logged and
    swallowed, matching every other subprocess call in this codebase
    (`widgets/calendar.py`, `.../reminders.py`, `.../git.py`, ...)."""
    script = (
        'if application "Music" is running then\n'
        f'  tell application "Music" to {action}\n'
        'else if application "Spotify" is running then\n'
        f'  tell application "Spotify" to {action}\n'
        'end if'
    )
    try:
        subprocess.run(["osascript", "-e", script], capture_output=True, timeout=timeout, check=False)
    except Exception:  # noqa: BLE001 - see docstring
        logger.debug("media_transport: osascript failed", exc_info=True)


def make_button_handler(
    is_media_active: Callable[[], bool],
    send: Callable[[str], None] = send_media_command,
) -> Callable[["device_mod.ButtonEvent"], None]:
    """Returns an `on_button` callback for `dashboard.loop.DashboardLoop`
    (its constructor's `on_button` parameter). Takes a `is_media_active`
    predicate rather than a `DashboardLoop` reference directly, so this
    module never needs to import `dashboard.loop` (keeping the
    cli.py-mediated wiring one-way, not circular) and so tests can drive
    it with a bare `lambda: True`/`lambda: False` instead of a real loop.
    `cli.py` wires `lambda: loop.active_preset_name == "media"` here.
    """

    def handler(evt: "device_mod.ButtonEvent") -> None:
        if evt.state != device_mod.BUTTON_STATE_PRESSED:
            return
        action = _ACTION_BY_BUTTON.get(evt.button)
        if action is None:
            return
        if not is_media_active():
            return
        send(action)

    return handler
