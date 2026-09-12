"""Dashboard config loading.

Uses PyYAML when it is importable (``pip install pyyaml``, or whatever the
environment already provides); otherwise falls back to a small,
dependency-free parser covering the documented subset of YAML this schema
actually needs, so the dashboard works with *no* third-party packages at
all beyond Pillow (which the renderer needs regardless).

Documented schema
------------------
::

    display:
      width: 320          # optional; --width/--height on the CLI win
      height: 240
      bpp: 1               # 1 or 2

    refresh_seconds: 60

    layout:
      grid:
        cols: 12
        rows: 8

    widgets:
      - type: clock
        at: [0, 0]         # [col, row], 0-indexed
        span: [6, 3]       # [width_cols, height_rows]
        options:
          format: "%H:%M"

The fallback parser supports: nested mappings via indentation, block lists
(``- item`` / ``- key: value``), flow lists (``[a, b, c]``), single/double
quoted and bare scalar strings, ints, floats, booleans, and ``null``/``~``.
It does **not** support YAML anchors/aliases, multi-document streams, flow
mappings (``{a: 1}``), block scalars (``|``/``>``), or tabs for indentation
-- none of which ``default_dashboard.yaml`` or this schema use. If your own
config needs one of those, install PyYAML (see REQUIREMENTS.md).
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

try:
    import yaml as _pyyaml  # type: ignore

    _HAVE_PYYAML = True
except Exception:  # pragma: no cover - exercised via _HAVE_PYYAML=False path
    _pyyaml = None
    _HAVE_PYYAML = False


class ConfigError(ValueError):
    """Raised for structurally invalid dashboard config."""


# ---------------------------------------------------------------------------
# Fallback subset-YAML parser
# ---------------------------------------------------------------------------

_BOOL_TRUE = {"true", "True", "TRUE", "yes", "Yes", "on", "On"}
_BOOL_FALSE = {"false", "False", "FALSE", "no", "No", "off", "Off"}
_NULLS = {"null", "Null", "NULL", "~", ""}

_INT_RE = re.compile(r"^[+-]?[0-9]+$")
_FLOAT_RE = re.compile(r"^[+-]?(\d+\.\d*|\.\d+|\d+)([eE][+-]?\d+)?$")


def _strip_comment(line: str) -> str:
    """Remove a trailing ``# comment``, respecting quotes."""
    in_single = False
    in_double = False
    for i, ch in enumerate(line):
        if ch == "'" and not in_double:
            in_single = not in_single
        elif ch == '"' and not in_single:
            in_double = not in_double
        elif ch == "#" and not in_single and not in_double:
            if i == 0 or line[i - 1] in (" ", "\t"):
                return line[:i]
    return line


def _tokenize(text: str) -> List[Tuple[int, str]]:
    lines: List[Tuple[int, str]] = []
    for raw in text.splitlines():
        if "\t" in raw[: len(raw) - len(raw.lstrip(" \t"))]:
            raise ConfigError("tabs are not supported for indentation in the fallback YAML parser")
        stripped = _strip_comment(raw).rstrip()
        if not stripped.strip():
            continue
        if stripped.strip() == "---":
            continue  # tolerate a leading document marker, ignore it
        indent = len(stripped) - len(stripped.lstrip(" "))
        content = stripped.strip()
        lines.append((indent, content))
    return lines


def _parse_scalar(text: str) -> Any:
    text = text.strip()
    if not text:
        return None
    if len(text) >= 2 and text[0] == text[-1] and text[0] in ("'", '"'):
        return text[1:-1]
    if text.startswith("[") and text.endswith("]"):
        inner = text[1:-1].strip()
        if not inner:
            return []
        return [_parse_scalar(part) for part in _split_flow(inner)]
    if text.startswith("{") and text.endswith("}"):
        inner = text[1:-1].strip()
        out: Dict[str, Any] = {}
        if inner:
            for part in _split_flow(inner):
                k, _, v = part.partition(":")
                out[k.strip().strip("'\"")] = _parse_scalar(v)
        return out
    if text in _BOOL_TRUE:
        return True
    if text in _BOOL_FALSE:
        return False
    if text in _NULLS:
        return None
    if _INT_RE.match(text):
        return int(text)
    if _FLOAT_RE.match(text):
        return float(text)
    return text


def _split_flow(inner: str) -> List[str]:
    """Split a flow-list/map interior on top-level commas (no nested [] {})."""
    parts: List[str] = []
    depth = 0
    cur = []
    in_single = in_double = False
    for ch in inner:
        if ch == "'" and not in_double:
            in_single = not in_single
        elif ch == '"' and not in_single:
            in_double = not in_double
        elif ch in "[{" and not in_single and not in_double:
            depth += 1
        elif ch in "]}" and not in_single and not in_double:
            depth -= 1
        if ch == "," and depth == 0 and not in_single and not in_double:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return [p.strip() for p in parts]


def _parse_map(lines: List[Tuple[int, str]], i: int, indent: int) -> Tuple[Dict[str, Any], int]:
    result: Dict[str, Any] = {}
    n = len(lines)
    while i < n:
        ind, content = lines[i]
        if ind != indent or content.startswith("- "):
            break
        if ":" not in content:
            raise ConfigError(f"expected 'key: value' in {content!r}")
        key, _, rest = content.partition(":")
        key = key.strip().strip("'\"")
        rest = rest.strip()
        if rest == "":
            i += 1
            if i < n and lines[i][0] > indent:
                val, i = _parse_block(lines, i, lines[i][0])
            else:
                val = None
            result[key] = val
        else:
            result[key] = _parse_scalar(rest)
            i += 1
    return result, i


def _parse_list(lines: List[Tuple[int, str]], i: int, indent: int) -> Tuple[List[Any], int]:
    result: List[Any] = []
    n = len(lines)
    while i < n:
        ind, content = lines[i]
        if ind != indent or not content.startswith("-"):
            break
        item = content[1:]
        lead_ws = len(item) - len(item.lstrip(" "))
        item = item.lstrip(" ")
        key_indent = indent + 1 + lead_ws
        if item == "":
            i += 1
            if i < n and lines[i][0] > indent:
                val, i = _parse_block(lines, i, lines[i][0])
            else:
                val = None
            result.append(val)
        elif ":" in item and not item.startswith(("[", "{", "'", '"')):
            sub_lines = [(key_indent, item)]
            j = i + 1
            while j < n and lines[j][0] > indent:
                sub_lines.append(lines[j])
                j += 1
            val, _ = _parse_map(sub_lines, 0, key_indent)
            result.append(val)
            i = j
        else:
            result.append(_parse_scalar(item))
            i += 1
    return result, i


def _parse_block(lines: List[Tuple[int, str]], i: int, indent: int) -> Tuple[Any, int]:
    if i >= len(lines):
        return None, i
    _, content = lines[i]
    if content.startswith("- "):
        return _parse_list(lines, i, indent)
    return _parse_map(lines, i, indent)


def parse_yaml_subset(text: str) -> Dict[str, Any]:
    """Parse the documented YAML subset into plain dict/list/scalar data."""
    lines = _tokenize(text)
    if not lines:
        return {}
    top_indent = lines[0][0]
    value, _ = _parse_block(lines, 0, top_indent)
    if not isinstance(value, dict):
        raise ConfigError("top-level dashboard config must be a mapping")
    return value


# ---------------------------------------------------------------------------
# Typed config
# ---------------------------------------------------------------------------


@dataclass
class DisplayConfig:
    width: Optional[int] = None
    height: Optional[int] = None
    bpp: int = 1


@dataclass
class GridConfig:
    cols: int = 12
    rows: int = 8


@dataclass
class WidgetConfig:
    type: str
    at: Tuple[int, int]
    span: Tuple[int, int]
    options: Dict[str, Any] = field(default_factory=dict)


@dataclass
class DashboardConfig:
    display: DisplayConfig
    refresh_seconds: int
    grid: GridConfig
    widgets: List[WidgetConfig]
    fonts: Dict[str, Any] = field(default_factory=dict)
    raw: Dict[str, Any] = field(default_factory=dict)


def _as_pair(value: Any, field_name: str) -> Tuple[int, int]:
    if not isinstance(value, (list, tuple)) or len(value) != 2:
        raise ConfigError(f"{field_name} must be a 2-element list, got {value!r}")
    a, b = value
    return int(a), int(b)


def _load_raw(text: str) -> Dict[str, Any]:
    if _HAVE_PYYAML:
        data = _pyyaml.safe_load(text)
        return data or {}
    return parse_yaml_subset(text)


def loads(text: str) -> DashboardConfig:
    """Parse dashboard YAML text into a `DashboardConfig`."""
    raw = _load_raw(text)
    if not isinstance(raw, dict):
        raise ConfigError("dashboard config must be a mapping at the top level")

    display_raw = raw.get("display", {}) or {}
    display = DisplayConfig(
        width=int(display_raw["width"]) if display_raw.get("width") is not None else None,
        height=int(display_raw["height"]) if display_raw.get("height") is not None else None,
        bpp=int(display_raw.get("bpp", 1)),
    )
    if display.bpp not in (1, 2):
        raise ConfigError(f"display.bpp must be 1 or 2, got {display.bpp}")

    grid_raw = ((raw.get("layout") or {}).get("grid")) or {}
    grid = GridConfig(cols=int(grid_raw.get("cols", 12)), rows=int(grid_raw.get("rows", 8)))
    if grid.cols <= 0 or grid.rows <= 0:
        raise ConfigError("layout.grid.cols and .rows must be positive")

    widgets_raw = raw.get("widgets") or []
    if not isinstance(widgets_raw, list):
        raise ConfigError("widgets must be a list")
    widgets: List[WidgetConfig] = []
    for idx, w in enumerate(widgets_raw):
        if not isinstance(w, dict) or "type" not in w:
            raise ConfigError(f"widgets[{idx}] must be a mapping with a 'type' key")
        at = _as_pair(w.get("at", [0, 0]), f"widgets[{idx}].at")
        span = _as_pair(w.get("span", [1, 1]), f"widgets[{idx}].span")
        if span[0] <= 0 or span[1] <= 0:
            raise ConfigError(f"widgets[{idx}].span must have positive dimensions")
        widgets.append(
            WidgetConfig(
                type=str(w["type"]),
                at=at,
                span=span,
                options=dict(w.get("options") or {}),
            )
        )

    return DashboardConfig(
        display=display,
        refresh_seconds=int(raw.get("refresh_seconds", 60)),
        grid=grid,
        widgets=widgets,
        fonts=dict(raw.get("fonts") or {}),
        raw=raw,
    )


def load(path: str) -> DashboardConfig:
    """Load and parse a dashboard YAML config file."""
    with open(path, "r", encoding="utf-8") as fh:
        return loads(fh.read())


def default_config_path() -> str:
    """Path to the bundled config `byok dashboard` uses when `--config` is
    not given: the layout tuned for the device's actual native panel
    (240x80 -- see that file's own header comment for why it, not
    `default_dashboard.yaml`, is the default)."""
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "default_dashboard_240x80.yaml")


def examples_dir() -> str:
    """Directory the bundled example configs (clock-focus.yaml, work.yaml,
    media.yaml, ...) ship in -- see docs/host-tools.md for what each
    one demonstrates."""
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "examples")


def list_examples() -> List[str]:
    """Base names (without `.yaml`) of every example config, sorted --
    what `byok dashboard --list-examples` prints, and what
    `resolve_config_path()` accepts as a bare name."""
    d = examples_dir()
    if not os.path.isdir(d):
        return []
    return sorted(
        os.path.splitext(name)[0]
        for name in os.listdir(d)
        if name.endswith(".yaml") and not name.startswith(".")
    )


def resolve_config_path(path: Optional[str]) -> str:
    """Resolve a `--config` argument to an actual file path.

    `None` -> the bundled default (`default_config_path()`).

    Otherwise, tried in order, first match wins:

      1. As given, relative to the current working directory (or absolute)
         -- the existing, unsurprising behavior for a path like
         `./my-dashboard.yaml` or `/abs/path.yaml`.
      2. The same, with a `.yaml` extension appended if it doesn't already
         have one -- still cwd-relative, so `--config my-dashboard` finds
         `./my-dashboard.yaml` too, not just an example.
      3. That extended name's *basename* (any directory component in
         `path` is dropped here, deliberately -- this step only ever means
         "look in the bundled examples/ dir", so `work`, `work.yaml`, and
         `examples/work.yaml` all land on the same file regardless of cwd)
         joined onto `examples_dir()`.

    Raises `FileNotFoundError` (with every attempted location named) if
    none of them exist -- `cli.py` turns that into the same clean
    `error: ...` + exit-2 shape `config.load()`'s own errors already get,
    rather than a raw traceback.
    """
    if path is None:
        return default_config_path()

    with_ext = path if path.endswith(".yaml") else path + ".yaml"
    candidates = [path]
    if with_ext != path:
        candidates.append(with_ext)
    candidates.append(os.path.join(examples_dir(), os.path.basename(with_ext)))

    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate

    tried = ", ".join(repr(c) for c in candidates)
    raise FileNotFoundError(
        f"no dashboard config found for {path!r} -- tried {tried}. "
        f"Run `byok dashboard --list-examples` to see the bundled examples."
    )
