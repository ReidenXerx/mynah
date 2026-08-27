"""whiz waveform-W logo — a custom-drawn monogram for the dictation icon.

A bold 'W' whose zigzag strokes read as an audio waveform — brand-specific
(whiz's own mark) instead of a generic SF Symbols microphone. Drawn as a
vector ``NSBezierPath`` with round caps and joins so it stays crisp from
16px (menu bar status item) up to 20px (pill indicator). No external image
assets — important for a pipx package that installs via ``pipx inject``.

The logo is monochrome (stroked in a single color), so the caller tints it
per state: gray idle / cyan listening / amber transcribing. Both the menu
bar status item and the floating pill indicator use this same drawing.
"""

from __future__ import annotations

import logging
from typing import Any

logger = logging.getLogger(__name__)

# W path in unit-square coordinates (0–1). Five points trace the W zigzag:
# top-left → first valley → middle peak → second valley → top-right.
# The middle peak sits lower than the outer tops so the shape reads as a
# waveform envelope, not just a bold letter.
_W_POINTS = (
    (0.10, 0.75),  # top-left
    (0.28, 0.25),  # first valley
    (0.50, 0.55),  # middle peak (lower than the outer tops)
    (0.72, 0.25),  # second valley
    (0.90, 0.75),  # top-right
)
# Stroke width as a fraction of the icon size — thick enough to read at 16px.
_W_STROKE = 0.14


def draw_whiz_logo(appkit: Any, rect: Any, color: Any) -> None:
    """Draw the whiz waveform-W logo into the current graphics context.

    Args:
        appkit: The ``AppKit`` module (passed in to avoid importing at module
            level, which would break import on non-macOS).
        rect: An ``NSRect`` describing where to draw (square aspect assumed;
            the logo uses ``min(width, height)`` as its side length).
        color: An ``NSColor`` to stroke with.
    """
    try:
        from Foundation import NSPoint

        origin = rect.origin
        size = min(rect.size.width, rect.size.height)
        stroke = size * _W_STROKE

        path = appkit.NSBezierPath.alloc().init()
        for i, (px, py) in enumerate(_W_POINTS):
            pt = NSPoint(origin.x + px * size, origin.y + py * size)
            if i == 0:
                path.moveToPoint_(pt)
            else:
                path.lineTo_(pt)

        path.setLineWidth_(stroke)
        path.setLineCapStyle_(appkit.NSCapStyleRound)
        path.setLineJoinStyle_(appkit.NSJoinStyleRound)
        color.set()
        path.stroke()
    except Exception:  # noqa: BLE001
        logger.debug("whiz logo draw failed", exc_info=True)


def whiz_logo_image(size: float, color: Any) -> Any:
    """Render the whiz logo as a tinted ``NSImage`` of the given square size.

    Returns an ``NSImage`` suitable for ``NSStatusItem.button().setImage_()``,
    or ``None`` if pyobjc/AppKit is unavailable (the caller falls back to no
    icon).
    """
    try:
        import AppKit
        from Foundation import NSRect, NSSize

        img = AppKit.NSImage.alloc().initWithSize_(NSSize(size, size))
        img.lockFocus()
        try:
            draw_whiz_logo(AppKit, NSRect((0, 0), (size, size)), color)
        finally:
            img.unlockFocus()
        return img
    except Exception:  # noqa: BLE001
        logger.debug("whiz logo image failed", exc_info=True)
        return None