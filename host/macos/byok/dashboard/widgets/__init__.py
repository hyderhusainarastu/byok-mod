"""Widget implementations. Importing this package registers every built-in
widget type (see `base.WIDGET_REGISTRY`) as a side effect -- `layout.py`
relies on that to resolve a config's `type: clock` etc. to a class."""

from __future__ import annotations

from .base import WIDGET_REGISTRY, RenderContext, Widget, register

from . import (  # noqa: F401  (imported for their @register side effects)
    calendar,
    clock,
    date,
    git,
    image,
    mac_stats,
    network,
    now_playing,
    qr,
    reminders,
    shell,
    text,
    writing,
)

__all__ = ["WIDGET_REGISTRY", "RenderContext", "Widget", "register"]
