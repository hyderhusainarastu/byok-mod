"""Dashboard presets -- named, orderable dashboard configs the device can
switch between live.

A "preset" is just a `DashboardConfig` (an ordinary file under
`presets/*.yaml`, same schema as `examples/*.yaml` -- see
`docs/host-tools.md` §4) plus a short `<=20`-char display name and a fixed
position in an ordered list, both declared in `presets/manifest.yaml`.
The manifest -- not alphabetical filename order, not registration order
-- is the single source of truth for preset *order*, because that order
is also the wire order: `byok dashboard` sends it to the device once at
connect via `SET_PRESETS` (`Device.set_presets()`, docs/protocol.md
§6.4b), and the device's own preset menu enumerates presets by that same
index. When the device later reports an `EVT_PRESET_CHANGED` event, or
`STATUS`/`EVT_STATUS`'s `flags` byte's preset-index bits (§6.1/§6.4b,
both v1.2/0.1.14), the index it sends is looked up against *this* list --
`dashboard/loop.py`'s `DashboardLoop` never re-derives the order itself.

Four presets ship (`presets/{clock,work,media,
writing}.yaml`) -- see `docs/host-tools.md`'s presets section for what
each one shows. `media.yaml` here deliberately has **no `qr` widget**
(an explicit owner decision, not an oversight -- the `qr` widget class
itself is untouched and still fully available to any config that wants
it, `examples/media.yaml` among them).
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import List, Optional

from . import config as config_mod

PRESETS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "presets")
MANIFEST_FILENAME = "manifest.yaml"
DEFAULT_MANIFEST_PATH = os.path.join(PRESETS_DIR, MANIFEST_FILENAME)

# docs/host-tools.md / device.py's _encode_set_presets(): the device's own
# menu line has limited width -- docs/protocol.md §6.4b's SET_PRESETS
# payload fixes this at exactly 20 bytes per name (a fixed-width,
# NUL-padded field, not a host-side guess -- reconciled against the
# firmware-side definition in docs/protocol.md; see
# device.py's `_encode_set_presets`/`SET_PRESETS_MAX_COUNT` and
# docs/host-tools.md's "Protocol reconciliation" section). Checked here at
# manifest-load time too (a build-time mistake in *our own* bundled
# manifest, unlike live device data, is worth failing loudly on rather
# than silently truncating) -- this is deliberately the same limit as
# `device.py`'s own wire-level truncation, just caught earlier and with a
# clearer error.
MAX_DISPLAY_NAME_LEN = 20

# docs/protocol.md §6.4b: SET_PRESETS's `count` field caps at 8 (a menu
# row limit) -- same reasoning as MAX_DISPLAY_NAME_LEN above: checked
# here too so a manifest mistake fails at load time with a clear message
# rather than surfacing later as a ValueError out of
# `device.py`'s `_encode_set_presets`.
MAX_PRESET_COUNT = 8


class PresetError(ValueError):
    """Raised for a structurally invalid presets manifest, or a display
    name over MAX_DISPLAY_NAME_LEN -- both are bugs in *our own* bundled
    data, never something a live device sends, so (unlike every widget
    Provider in this package) this is allowed to raise rather than
    degrade."""


@dataclass(frozen=True)
class PresetEntry:
    name: str      # short machine key, e.g. "clock" -- what --preset NAME matches
    display: str   # <=20-char label, what SET_PRESETS puts on the wire
    path: str      # resolved absolute path to this preset's config YAML


def load_manifest(manifest_path: str = DEFAULT_MANIFEST_PATH) -> List[PresetEntry]:
    """Loads and validates `manifest.yaml`, in file order (the wire/menu
    order -- see module docstring). Raises `PresetError` for anything
    structurally wrong: not a list, a missing `name`/`display`/`file`
    key, a `display` over `MAX_DISPLAY_NAME_LEN`, a duplicate `name`, or a
    `file` that doesn't resolve to an existing preset config next to the
    manifest. Raises `FileNotFoundError` if `manifest_path` itself is
    missing (matches `config.load()`'s own behavior for a missing file --
    an `OSError` subclass, not a `PresetError`, since that's a different
    failure shape cli.py already knows how to report cleanly)."""
    with open(manifest_path, "r", encoding="utf-8") as fh:
        text = fh.read()
    raw = config_mod._load_raw(text)
    if not isinstance(raw, dict):
        raise PresetError(f"{manifest_path}: top-level manifest must be a mapping")

    items = raw.get("presets")
    if not isinstance(items, list) or not items:
        raise PresetError(f"{manifest_path}: 'presets' must be a non-empty list")
    if len(items) > MAX_PRESET_COUNT:
        raise PresetError(
            f"{manifest_path}: {len(items)} presets exceeds the device's "
            f"{MAX_PRESET_COUNT}-preset SET_PRESETS limit"
        )

    base_dir = os.path.dirname(os.path.abspath(manifest_path))
    entries: List[PresetEntry] = []
    seen_names = set()
    for idx, item in enumerate(items):
        if not isinstance(item, dict):
            raise PresetError(f"{manifest_path}: presets[{idx}] must be a mapping")
        for key in ("name", "display", "file"):
            if not item.get(key):
                raise PresetError(f"{manifest_path}: presets[{idx}] is missing required key {key!r}")
        name = str(item["name"])
        display = str(item["display"])
        file_name = str(item["file"])
        if name in seen_names:
            raise PresetError(f"{manifest_path}: duplicate preset name {name!r}")
        seen_names.add(name)
        if len(display.encode("utf-8")) > MAX_DISPLAY_NAME_LEN:
            raise PresetError(
                f"{manifest_path}: presets[{idx}] display name {display!r} exceeds "
                f"{MAX_DISPLAY_NAME_LEN} bytes"
            )
        path = os.path.join(base_dir, file_name)
        if not os.path.isfile(path):
            raise PresetError(f"{manifest_path}: presets[{idx}] file {file_name!r} not found at {path}")
        entries.append(PresetEntry(name=name, display=display, path=path))

    return entries


def default_presets() -> List[PresetEntry]:
    """The bundled presets (`presets/manifest.yaml`), in wire order."""
    return load_manifest(DEFAULT_MANIFEST_PATH)


def find_preset(name: str, entries: Optional[List[PresetEntry]] = None) -> PresetEntry:
    """Looks up a preset by its short `name` (case-insensitive, matching
    `--config`'s own bare-name convenience) -- what `--preset NAME`
    resolves against. Raises `PresetError` naming every available preset
    if `name` doesn't match one."""
    entries = entries if entries is not None else default_presets()
    for entry in entries:
        if entry.name.lower() == name.lower():
            return entry
    available = ", ".join(e.name for e in entries)
    raise PresetError(f"no preset named {name!r} -- available: {available}")


def load_preset_config(entry: PresetEntry) -> config_mod.DashboardConfig:
    return config_mod.load(entry.path)


def display_names(entries: List[PresetEntry]) -> List[str]:
    """What `Device.set_presets()` sends -- display names in manifest
    (wire/menu) order."""
    return [e.display for e in entries]
