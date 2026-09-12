"""Font loading/caching for dashboard widgets.

Resolution order for a requested pixel size:

1. An explicit TTF/OTF path from config (``fonts.path`` at the top level,
   or a per-widget ``options.font``), via ``ImageFont.truetype``.
2. A handful of fonts that ship with every macOS install (Helvetica,
   Menlo), tried via ``ImageFont.truetype`` -- best-effort, silently
   skipped if the file isn't there (e.g. running these tests on Linux CI).
3. PIL's built-in default font at the requested size
   (``ImageFont.load_default(size=...)``, Pillow >= 10.1). This is a real
   scalable bitmap font and is what most installs will actually use.
4. PIL's built-in default font at its one fixed native size (very old
   Pillow without the ``size=`` kwarg), nearest-neighbour upscaled to the
   requested size so text stays crisp (no blur) even though it's blocky --
   this is the "8x8/8x16 bitmap font fallback" of last resort.

Every path is wrapped so a missing/broken font never raises out of
`FontSet.get()` -- worst case you get PIL's default font.
"""

from __future__ import annotations

from typing import Dict, Optional, Tuple

from PIL import Image, ImageFont

_MAC_SYSTEM_FONTS = (
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Monaco.ttf",
)


class FontSet:
    """Caches loaded fonts by (path_or_None, size)."""

    def __init__(self, ttf_path: Optional[str] = None):
        self.ttf_path = ttf_path
        self._cache: Dict[Tuple[Optional[str], int], ImageFont.FreeTypeFont] = {}
        self._default_native: Optional[ImageFont.ImageFont] = None
        self._default_native_size: Optional[int] = None

    def get(self, size: int, path: Optional[str] = None) -> ImageFont.ImageFont:
        size = max(1, int(size))
        chosen = path or self.ttf_path
        key = (chosen, size)
        cached = self._cache.get(key)
        if cached is not None:
            return cached

        font = self._load_truetype(chosen, size)
        if font is None:
            for candidate in _MAC_SYSTEM_FONTS:
                font = self._load_truetype(candidate, size)
                if font is not None:
                    break
        if font is None:
            font = self._load_default_scalable(size)
        if font is None:
            font = self._load_default_native()

        self._cache[key] = font
        return font

    @staticmethod
    def _load_truetype(path: Optional[str], size: int) -> Optional[ImageFont.ImageFont]:
        if not path:
            return None
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            return None

    @staticmethod
    def _load_default_scalable(size: int) -> Optional[ImageFont.ImageFont]:
        try:
            return ImageFont.load_default(size=size)
        except TypeError:
            return None  # Pillow too old to take a size kwarg
        except Exception:
            return None

    def _load_default_native(self) -> ImageFont.ImageFont:
        if self._default_native is None:
            self._default_native = ImageFont.load_default()
        return self._default_native


def text_size(draw, text: str, font: ImageFont.ImageFont) -> Tuple[int, int]:
    """(width, height) of `text` rendered in `font`, across Pillow versions."""
    if hasattr(draw, "textbbox"):
        l, t, r, b = draw.textbbox((0, 0), text, font=font)
        return (r - l, b - t)
    if hasattr(font, "getsize"):
        return font.getsize(text)  # pragma: no cover - very old Pillow
    return (len(text) * 6, 11)  # pragma: no cover - last-ditch guess


_SMALL_ROW_PX = 12  # rows this short default to text_case="upper"; see resolve_text_case()


def resolve_text_case(ctx, default_row_px: int = _SMALL_ROW_PX) -> str:
    """Resolve a widget's effective ``text_case`` option: ``"upper"`` or ``"as-is"``.

    An explicit ``options.text_case`` always wins. Otherwise this defaults to
    ``"upper"`` for rows shorter than `default_row_px` and ``"as-is"``
    otherwise: anti-aliased small text gets hard-thresholded to 1-bit with no
    dithering (see ``render.py``'s ``DITHERED_WIDGET_TYPES``), and thin
    lowercase strokes -- a 'c', an 'e''s counter, an 'a' -- are the first
    thing lost at those sizes even once a widget's own auto-fit font-size cap
    is otherwise healthy. Uppercase glyphs are built from thicker, straighter
    strokes and hold up far better under that thresholding. See
    docs/troubleshooting.md.
    """
    value = ctx.options.get("text_case")
    if value in ("upper", "as-is"):
        return value
    return "upper" if ctx.height < default_row_px else "as-is"


def apply_text_case(text: str, ctx) -> str:
    """Apply `ctx`'s resolved text_case (see `resolve_text_case`) to `text`."""
    return text.upper() if resolve_text_case(ctx) == "upper" else text


def fit_font_size(draw, text: str, fonts: FontSet, max_width: int, max_height: int,
                   min_size: int = 6, max_size: int = 200, path: Optional[str] = None) -> ImageFont.ImageFont:
    """Binary-search the largest font size for which `text` fits the box."""
    if not text:
        return fonts.get(min_size, path=path)
    lo, hi = min_size, max_size
    best = fonts.get(min_size, path=path)
    while lo <= hi:
        mid = (lo + hi) // 2
        font = fonts.get(mid, path=path)
        w, h = text_size(draw, text, font)
        if w <= max_width and h <= max_height:
            best = font
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def ellipsize(draw, text: str, font, max_width: int) -> str:
    """Truncate `text` to fit `max_width` px at `font`, appending an
    ellipsis ("…") if truncation was needed. Binary-searches the cut point
    (same technique as `fit_font_size`) rather than trimming one character
    at a time -- matters once this runs per dashboard-refresh cycle on
    every list line a widget draws.

    A no-op (returns `text` unchanged) when it already fits. If even a bare
    "…" doesn't fit `max_width`, returns "" -- never raises, never returns
    something wider than the box. Used anywhere a widget's own auto-fit
    font size (`fit_font_size`) still leaves a line too wide for its tile
    at the size floor (`min_size`) -- e.g. a very long git branch name or
    now-playing track title -- so overflow degrades to a readable "…" cut
    rather than an abrupt clip at the tile's own pixel edge.
    """
    if not text or text_size(draw, text, font)[0] <= max_width:
        return text
    if text_size(draw, "…", font)[0] > max_width:
        return ""
    lo, hi = 0, len(text)
    best = ""
    while lo <= hi:
        mid = (lo + hi) // 2
        candidate = text[:mid] + "…"
        if text_size(draw, candidate, font)[0] <= max_width:
            best = candidate
            lo = mid + 1
        else:
            hi = mid - 1
    return best
