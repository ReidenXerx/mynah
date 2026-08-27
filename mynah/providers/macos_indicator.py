"""MacIndicator — a floating "listening" overlay for macOS dictation.

A small, borderless, always-on-top, click-through ``NSPanel`` with a
custom ``NSView`` that draws:
- a circular mic badge (filled, with a subtle ring)
- a live volume curve that responds to mic amplitude

The indicator runs on the main thread inside the AppKit event loop
(NSApplication). The engine feeds it per-chunk RMS amplitude (0.0–1.0)
from the audio thread via ``update_level()``, which is thread-safe
through AppKit's main-thread dispatch.

States change the badge color:
- "listening"    → pulsing cyan
- "transcribing" → amber (steady)
- "idle"         → dimmed gray
"""

from __future__ import annotations

import logging
import math
from typing import Any

from whiz.dictate.providers.base import DictationIndicator

logger = logging.getLogger(__name__)

# Overlay dimensions.
_PANEL_WIDTH = 120
_PANEL_HEIGHT = 120
_PANEL_CORNER_RADIUS = 20.0

# Colors (RGBA floats 0–1).
_COLOR_LISTENING = (0.2, 0.8, 0.95, 1.0)       # cyan
_COLOR_TRANSCRIBING = (0.95, 0.7, 0.2, 1.0)    # amber
_COLOR_IDLE = (0.5, 0.5, 0.55, 0.6)            # dimmed gray


class MacIndicator(DictationIndicator):
    """A floating dictation indicator overlay (macOS NSPanel + custom NSView)."""

    def __init__(self) -> None:
        self._panel: Any = None
        self._view: Any = None
        self._level: float = 0.0
        self._state: str = "idle"

    def setup(self) -> None:
        """Create the NSPanel on the main thread before the run loop starts.

        AppKit requires ``NSWindow``/``NSPanel`` to be instantiated on the
        main thread; calling ``_create_panel()`` from the hotkey listener
        thread raises ``NSInternalInconsistencyException``. The engine calls
        this once on the main thread (inside ``_run_with_appkit``) so the
        panel exists before any ``show()``.
        """
        self._ensure_panel()

    def show(self) -> None:
        """Display the overlay (main-thread-safe).

        The panel is created eagerly in ``setup()``. Ordering it to the
        front is an AppKit UI op; dispatch it to the main thread so a call
        from the hotkey/transcribe threads is safe.
        """
        if self._panel is None:
            return
        try:
            self._panel.performSelectorOnMainThread_withObject_waitUntilDone_(
                "orderFrontRegardless", None, False
            )
        except Exception:  # noqa: BLE001
            pass

    def hide(self) -> None:
        """Dismiss the overlay (main-thread-safe)."""
        if self._panel is None:
            return
        try:
            self._panel.performSelectorOnMainThread_withObject_waitUntilDone_(
                "orderOut:", None, False
            )
        except Exception:  # noqa: BLE001
            pass

    def update_level(self, level: float) -> None:
        """Feed a live mic amplitude in [0.0, 1.0] to animate the volume curve.

        Thread-safe: dispatches the actual view update to the main thread.
        """
        self._level = max(0.0, min(1.0, level))
        if self._view is not None:
            # Dispatch to main thread for AppKit safety.
            try:
                from Foundation import NSMutableArray
                # performSelectorOnMainThread_ is the simplest safe path.
                self._view.performSelectorOnMainThread_withObject_waitUntilDone_(
                    "_whiz_update_display", None, False
                )
            except Exception:  # noqa: BLE001
                # If dispatch fails, the view just won't update this frame —
                # not worth crashing dictation over.
                pass

    def set_state(self, state: str) -> None:
        """Notify the indicator of a state change (listening/transcribing/idle)."""
        self._state = state
        if self._view is not None:
            try:
                self._view.performSelectorOnMainThread_withObject_waitUntilDone_(
                    "_whiz_set_state", None, False
                )
            except Exception:  # noqa: BLE001
                pass

    # ---------- panel/view setup ----------

    def _ensure_panel(self) -> None:
        """Lazily create the NSPanel + custom view (idempotent)."""
        if self._panel is not None:
            return
        try:
            self._create_panel()
        except ImportError:
            logger.warning(
                "PyObjC not available — dictation indicator disabled. "
                "Install: pipx inject whiz 'whiz[dictate]'"
            )
            self._panel = None
        except Exception:  # noqa: BLE001
            logger.warning("Could not create dictation indicator", exc_info=True)
            self._panel = None

    def _create_panel(self) -> None:
        """Build the NSPanel + WhizIndicatorView. Imports pyobjc lazily."""
        import AppKit
        from Foundation import NSRect, NSPoint, NSSize

        # Position: bottom-center of the screen, slightly inset.
        screen_frame = AppKit.NSScreen.mainScreen().frame()
        x = (screen_frame.size.width - _PANEL_WIDTH) / 2
        y = 80  # 80px from the bottom
        frame = NSRect(NSPoint(x, y), NSSize(_PANEL_WIDTH, _PANEL_HEIGHT))

        # NSPanel with borderless, non-activating, floating level.
        style = AppKit.NSWindowStyleMaskBorderless
        backing = AppKit.NSBackingStoreBuffered
        self._panel = AppKit.NSPanel.alloc().initWithContentRect_styleMask_backing_defer_(
            frame, style, backing, False
        )
        self._panel.setLevel_(AppKit.NSFloatingWindowLevel)
        self._panel.setOpaque_(False)
        self._panel.setBackgroundColor_(AppKit.NSColor.clearColor())
        self._panel.setHasShadow_(True)
        # Click-through: ignore mouse events so the panel doesn't steal focus.
        self._panel.setIgnoresMouseEvents_(True)
        # Don't activate the app when the panel shows.
        self._panel.setBecomesKeyOnlyIfNeeded_(True)

        # Custom view that draws the badge + volume curve. Use the real
        # ObjC NSView subclass (created at import time on macOS) — only
        # ObjC classes have .alloc(); the plain Python WhizIndicatorView
        # is just the methods holder the subclass delegates to.
        view_cls = _get_objc_view_class()
        self._view = view_cls.alloc().initWithFrame_(frame)
        self._view._indicator = self  # back-reference for state/level reads
        self._panel.setContentView_(self._view)


class WhizIndicatorView:
    """Custom NSView drawing the mic badge + animated volume curve.

    Defined as a Python class; we attach it to an NSView subclass at
    runtime via objc. This avoids requiring a compiled PyObjC subclass
    at import time and keeps the module importable without AppKit.
    """

    # These attributes are set after alloc().initWithFrame_ by the runtime
    # glue; declared here for type checkers.
    _indicator: "MacIndicator"

    def initWithFrame_(self, frame):
        """NSView initializer. ``self`` is the ObjC instance."""
        self = _objc_super_init(self, frame)
        if self is not None:
            self._indicator = None
        return self

    def _whiz_update_display(self) -> None:
        """Trigger a redraw on the main thread (called via performSelector)."""
        try:
            self.setNeedsDisplay_(True)
        except Exception:  # noqa: BLE001
            pass

    def _whiz_set_state(self) -> None:
        """State changed — redraw."""
        try:
            self.setNeedsDisplay_(True)
        except Exception:  # noqa: BLE001
            pass

    def drawRect_(self, rect) -> None:
        """NSView draw — paints the circular badge + volume curve."""
        try:
            import AppKit
            from Foundation import NSRect, NSBezierPath

            bounds = self.bounds()
            w = bounds.size.width
            h = bounds.size.height
            cx = w / 2
            cy = h / 2

            # Pick the color for the current state.
            ind = getattr(self, "_indicator", None)
            state = ind._state if ind else "idle"
            level = ind._level if ind else 0.0
            color = {
                "listening": _COLOR_LISTENING,
                "transcribing": _COLOR_TRANSCRIBING,
            }.get(state, _COLOR_IDLE)

            # Clear background (transparent panel).
            AppKit.NSColor.clearColor().set()
            AppKit.NSRectFill(NSRect((0, 0), (w, h)))

            # Draw the rounded panel background (subtle dark blur look).
            bg_path = AppKit.NSBezierPath.bezierPathWithRoundedRect_xRadius_yRadius_(
                NSRect((0, 0), (w, h)), _PANEL_CORNER_RADIUS, _PANEL_CORNER_RADIUS
            )
            AppKit.NSColor.colorWithCalibratedRed_green_blue_alpha_(
                0.1, 0.1, 0.12, 0.85
            ).set()
            bg_path.fill()

            # Draw the circular mic badge.
            badge_radius = 22
            badge = AppKit.NSBezierPath.bezierPathWithRoundedRect_xRadius_yRadius_(
                NSRect((cx - badge_radius, cy - badge_radius),
                       (badge_radius * 2, badge_radius * 2)),
                badge_radius, badge_radius
            )
            AppKit.NSColor.colorWithCalibratedRed_green_blue_alpha_(*color).set()
            badge.fill()

            # Draw the volume curve: an arc whose sweep grows with `level`.
            # The arc is drawn around the badge, like a ring that fills up.
            if level > 0.01:
                ring_radius = badge_radius + 8
                # Arc from -90° (top) sweeping clockwise proportional to level.
                start = -math.pi / 2
                sweep = level * 2 * math.pi
                # Draw as a series of short line segments (simple, no CGPath).
                segments = max(4, int(sweep / 0.1))
                ring = AppKit.NSBezierPath.bezierPath()
                for i in range(segments + 1):
                    angle = start + sweep * (i / segments)
                    px = cx + ring_radius * math.cos(angle)
                    py = cy + ring_radius * math.sin(angle)
                    if i == 0:
                        ring.moveToPoint_((px, py))
                    else:
                        ring.lineToPoint_((px, py))
                ring.setLineWidth_(3.0)
                AppKit.NSColor.colorWithCalibratedRed_green_blue_alpha_(*color).set()
                ring.stroke()

            # Draw a simple mic glyph (a rounded rectangle "stem" + a base line).
            stem = AppKit.NSBezierPath.bezierPathWithRoundedRect_xRadius_yRadius_(
                NSRect((cx - 5, cy - 6), (10, 16)), 5, 5
            )
            AppKit.NSColor.whiteColor().set()
            stem.fill()
            # Mic base arc.
            base = AppKit.NSBezierPath.bezierPath()
            base.moveToPoint_((cx - 10, cy - 4))
            base.lineToPoint_((cx + 10, cy - 4))
            base.setLineWidth_(2.0)
            base.stroke()
        except Exception:  # noqa: BLE001
            logger.debug("indicator draw failed", exc_info=True)


# ---------- ObjC runtime glue ----------
# We create a real NSView subclass at import time (only on macOS where
# AppKit exists) so drawRect_ etc. are callable from the ObjC runtime.
# On non-macOS, the class stays a plain Python class and _ensure_panel
# catches the ImportError.
#
# Design:
# - WhizIndicatorView (above) holds the pure-Python draw/state methods.
# - _WhizIndicatorViewImpl is a real NSView (ObjC) subclass whose methods
#   delegate to WhizIndicatorView.* (drawRect, _whiz_update_display, ...).
# - _get_objc_view_class() returns the ObjC subclass, creating it once
#   (idempotent). _create_panel calls .alloc().initWithFrame_() on it.
# - On non-macOS / no pyobjc, _get_objc_view_class() raises ImportError,
#   which _ensure_panel catches and degrades to no indicator.

_OBJC_VIEW_CLASS = None


def _create_objc_view_class():
    """Create (once) an NSView subclass that delegates to WhizIndicatorView methods."""
    global _OBJC_VIEW_CLASS
    if _OBJC_VIEW_CLASS is not None:
        return _OBJC_VIEW_CLASS
    import objc
    from AppKit import NSView

    class _WhizIndicatorViewImpl(NSView):
        def initWithFrame_(self, frame):
            self = objc.super(_WhizIndicatorViewImpl, self).initWithFrame_(frame)
            if self is not None:
                self._indicator = None
            return self

        def drawRect_(self, rect):
            WhizIndicatorView.drawRect_(self, rect)

        def _whiz_update_display(self):
            self.setNeedsDisplay_(True)

        def _whiz_set_state(self):
            self.setNeedsDisplay_(True)

    _OBJC_VIEW_CLASS = _WhizIndicatorViewImpl
    return _OBJC_VIEW_CLASS


def _get_objc_view_class():
    """Return the ObjC NSView subclass for the indicator view.

    Creates it on first call (idempotent). Raises ImportError on non-macOS
    or when pyobjc/AppKit is unavailable, so _ensure_panel can degrade.
    """
    cls = _create_objc_view_class()
    if cls is None:
        raise ImportError("ObjC NSView subclass unavailable")
    return cls


# Eagerly create the ObjC subclass at import time on macOS so .alloc() is
# available before _create_panel runs. On non-macOS the import fails and
# _OBJC_VIEW_CLASS stays None; _get_objc_view_class() raises ImportError.
try:
    _create_objc_view_class()
except ImportError:
    pass
except Exception:  # noqa: BLE001 - never fail import on a pyobjc quirk
    logger.debug("indicator ObjC view class not created", exc_info=True)
