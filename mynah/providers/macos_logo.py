"""Mynah's mark, drawn as a vector — the round-headed bird with the stubby bill.

The same geometry as `MynahLogo` in the Swift app and
`docs/assets/mynah-glyph.svg`: the coordinates are GENERATED from one source, so
the copies cannot drift apart. Vector rather than an image
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
_EYE = (0.5385, 0.4904, 0.1154)


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
        # the head
        path.moveToPoint_(at(0.6731, 0.4615))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.4231, 0.2115), at(0.6635, 0.3269), at(0.5673, 0.2212))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.0673, 0.5481), at(0.2308, 0.2019), at(0.0673, 0.3558))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.2885, 0.875), at(0.0673, 0.7019), at(0.1538, 0.8269))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.625, 0.8077), at(0.4135, 0.9231), at(0.5481, 0.8942))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.6731, 0.4615), at(0.6635, 0.7212), at(0.6779, 0.5865))
        path.closePath()
        # the crest, three feathers
        path.moveToPoint_(at(0.3173, 0.2404))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.3462, 0.0577), at(0.2788, 0.1538), at(0.2885, 0.0962))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.4327, 0.2019), at(0.3558, 0.125), at(0.3846, 0.1731))
        path.closePath()
        path.moveToPoint_(at(0.4519, 0.1923))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.5288, 0.0481), at(0.4327, 0.1058), at(0.4615, 0.0577))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.5577, 0.2115), at(0.5192, 0.1154), at(0.5288, 0.1635))
        path.closePath()
        path.moveToPoint_(at(0.2404, 0.2885))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.1923, 0.125), at(0.1731, 0.2308), at(0.1538, 0.1731))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.3077, 0.2404), at(0.2212, 0.1827), at(0.2596, 0.2212))
        path.closePath()
        # the bill: short, straight, blunt
        path.moveToPoint_(at(0.6635, 0.4231))
        path.lineToPoint_(at(0.8942, 0.4904))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.8942, 0.5673), at(0.9327, 0.5048), at(0.9327, 0.5529))
        path.lineToPoint_(at(0.6635, 0.625))
        path.curveToPoint_controlPoint1_controlPoint2_(at(0.6635, 0.4231), at(0.6442, 0.5577), at(0.6442, 0.4904))
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

