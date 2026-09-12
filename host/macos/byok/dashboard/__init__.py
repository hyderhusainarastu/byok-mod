"""byok.dashboard -- device-independent Mac dashboard renderer.

Renders a configurable grid of widgets (clock, date, mac stats, calendar,
now playing, QR codes, ...) into a single PIL 'L' (8-bit grayscale) image
sized for the target display, ready for a caller to hand to
``byok.render.quantize_levels`` / ``pack_framebuffer`` for the wire format.

This subpackage does not know anything about the device, the transport, or
the wire protocol -- see docs/protocol.md and byok.render for that. It only
needs a width, a height, a bit depth, and a config file.

Quick start::

    python3 -m byok.dashboard.preview --config default_dashboard.yaml \
        --width 320 --height 240 --out preview.png

Layout:
    config.py          YAML (or dependency-free subset-YAML) config loader
    widgets/            one module per widget type, each a Widget subclass
    fonts.py            font loading/caching (TTF or PIL bitmap fallback)
    layout.py           grid layout compositor
    render.py           compose + quantize to 1bpp/2bpp PIL image
    default_dashboard.yaml  a polished, geometry-agnostic default config
    preview.py          CLI to render a config to a PNG for review

See REQUIREMENTS.md in this directory for optional third-party dependencies
(none are hard-required; the package degrades gracefully without them).
"""

from __future__ import annotations

__all__ = ["__version__"]
__version__ = "0.1.0"
