"""Mynah's mark, drawn as a vector — the bird head with the beak open mid-word.

The same geometry as `MynahLogo` in the Swift app and `docs/assets/mynah-glyph.svg`:
one unit-square path, traced from the same drawing, so the three are the same
bird rather than three drifting copies of one. Vector rather than an image
asset, because a pipx install ships no resources — and because it stays crisp
from a 16px menu bar item up to the pill.

The eye is a hole, not a second colour: filled with the even-odd rule the mark
reads as one silhouette at any size and in any tint, so the caller can colour it
per state (idle, listening, transcribing) without knowing anything about it.
"""

from __future__ import annotations

import logging
from typing import Any

logger = logging.getLogger(__name__)

# Where the eye sits, in the same unit square, and how big it is.
_EYE = (0.551, 0.344, 0.043)


def draw_mynah_logo(appkit: Any, rect: Any, color: Any) -> None:
    """Fill the mark into the current graphics context.

    Args:
        appkit: The ``AppKit`` module, passed in so this file imports anywhere.
        rect: An ``NSRect`` to draw into; the mark uses ``min(width, height)``.
        color: An ``NSColor`` to fill with.
    """
    try:
        from Foundation import NSPoint, NSRect

        # NSRect in pyobjc is a struct of tuples: rect.origin is (x, y) and
        # rect.size is (w, h), so index them rather than reaching for .x/.width.
        ox, oy = rect.origin
        rw, rh = rect.size
        side = min(rw, rh)

        def at(x: float, y: float) -> Any:
            # The drawing is defined top-left down; AppKit's origin is bottom-left.
            return NSPoint(ox + x * side, oy + (1.0 - y) * side)

        path = appkit.NSBezierPath.alloc().init()
        # crest
        path.moveToPoint_(at(0.3594, 0.1602))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.3789, 0.043), at(0.3359, 0.1133), at(0.3438, 0.0703))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.4375, 0.1367), at(0.3828, 0.0859), at(0.4023, 0.1172))
        path.closePath()
        # head and shoulder
        path.moveToPoint_(at(0.4062, 0.1562))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.7109, 0.4453), at(0.5781, 0.1562), at(0.7109, 0.2812))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.6953, 0.5352), at(0.7109, 0.4766), at(0.707, 0.5078))
        path.lineToPoint_(at(0.7812, 0.5508))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.793, 0.5977), at(0.8047, 0.5547), at(0.8125, 0.582))
        path.lineToPoint_(at(0.6758, 0.6953))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.5, 0.7578), at(0.6289, 0.7344), at(0.5664, 0.7578))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.1797, 0.4453), at(0.3203, 0.7578), at(0.1797, 0.6211))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.4062, 0.1562), at(0.1797, 0.2695), at(0.2266, 0.1562))
        path.closePath()
        # beak, open mid-word
        path.moveToPoint_(at(0.6797, 0.375))
        path.lineToPoint_(at(0.9219, 0.3984))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.9336, 0.4297), at(0.9375, 0.3984), at(0.9453, 0.418))
        path.lineToPoint_(at(0.8789, 0.4766))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.8516, 0.4805), at(0.8711, 0.4844), at(0.8594, 0.4844))
        path.lineToPoint_(at(0.6797, 0.4219))
        path.closePath()
        path.moveToPoint_(at(0.6875, 0.4766))
        path.lineToPoint_(at(0.8906, 0.5078))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.8906, 0.5352), at(0.9062, 0.5117), at(0.9062, 0.5312))
        path.lineToPoint_(at(0.7734, 0.5664))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.7539, 0.5586), at(0.7656, 0.5703), at(0.7578, 0.5664))
        path.lineToPoint_(at(0.6875, 0.5))
        path.closePath()

        # the eye, as a hole in the silhouette
        ex, ey, er = _EYE
        centre = at(ex, ey)
        radius = er * side
        path.appendBezierPathWithOvalInRect_(
            NSRect((centre.x - radius, centre.y - radius), (radius * 2, radius * 2))
        )
        path.setWindingRule_(appkit.NSWindingRuleEvenOdd)

        color.set()
        path.fill()
    except Exception:  # noqa: BLE001
        logger.debug("mynah logo draw failed", exc_info=True)

