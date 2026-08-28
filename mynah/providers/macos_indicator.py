"""MacIndicator — a floating "listening" pill overlay for macOS dictation.

A compact, borderless, always-on-top, click-through ``NSPanel`` containing
an ``NSVisualEffectView`` (HUD-window vibrancy material) so the overlay
blurs the content behind it — the native macOS HUD look — instead of an
opaque dark box. Inside the pill:

- the whiz waveform-W logo (a custom-drawn monogram), tinted by state
  (cyan listening / amber transcribing / gray idle)
- 5 live waveform bars whose heights track the mic amplitude

The indicator runs on the main thread inside the AppKit event loop
(``NSApplication``). The engine feeds it per-chunk RMS amplitude (0.0–1.0)
from the audio thread via ``update_level()``, which is thread-safe through
AppKit's main-thread dispatch. Show/hide animate via an opacity fade
(``alphaValue``) so the pill eases in/out rather than popping.

States change the logo tint:
- "listening"    → cyan
- "transcribing" → amber
- "idle"         → dimmed gray
"""

from __future__ import annotations

import logging
from typing import Any

from whiz.dictate.providers.base import DictationIndicator

logger = logging.getLogger(__name__)

# Overlay dimensions — a short horizontal pill.
_PANEL_WIDTH = 168
_PANEL_HEIGHT = 44
_PANEL_CORNER_RADIUS = _PANEL_HEIGHT / 2  # fully rounded ends

# Colors (RGBA floats 0–1).
_COLOR_LISTENING = (0.2, 0.8, 0.95, 1.0)       # cyan
_COLOR_TRANSCRIBING = (0.95, 0.7, 0.2, 1.0)   # amber
_COLOR_IDLE = (0.5, 0.5, 0.55, 0.85)          # dimmed gray

# Waveform bars.
_BAR_COUNT = 5
_BAR_WIDTH = 4
_BAR_GAP = 6
_BAR_MAX_HEIGHT = 22
_BAR_MIN_HEIGHT = 4
_BAR_CORNER = _BAR_WIDTH / 2

# Fade animation duration (seconds).
_FADE_SECONDS = 0.18


class MacIndicator(DictationIndicator):
    """A floating dictation indicator overlay (macOS NSPanel + vibrancy pill)."""

    def __init__(self) -> None:
        self._panel: Any = None
        self._view: Any = None
        self._level: float = 0.0
        self._state: str = "idle"
        self._visible: bool = False

    def setup(self) -> None:
        """Create the NSPanel on the main thread before the run loop starts.

        AppKit requires ``NSWindow``/``NSPanel`` to be instantiated on the
        main thread; calling ``_create_panel()`` from the hotkey listener
        thread raises ``NSInternalInconsistencyException``. The engine calls
        this once on the main thread (inside ``_run_with_appkit``) so the
        panel exists before any ``show()``.
        """
        logger.debug("MacIndicator.setup() called")
        self._ensure_panel()
        logger.debug(
            "MacIndicator.setup() done: panel=%s view=%s",
            self._panel is not None, self._view is not None,
        )

    def show(self) -> None:
        """Display the overlay with an opacity fade (main-thread-safe).

        The panel is created eagerly in ``setup()``. Ordering it to the
        front is an AppKit UI op; dispatch it to the main thread so a call
        from the hotkey/transcribe threads is safe.

        The ``whizFadeIn:``/``whizFadeOut:`` selectors live on the indicator
        view (``_WhizIndicatorViewImpl``), not on ``NSPanel`` — so dispatch
        to ``self._view`` (matching ``update_level``/``set_state``). The
        view's ``window()`` resolves the panel at run time.
        """
        if self._view is None:
            logger.warning("indicator show() but _view is None — panel not created")
            return
        self._visible = True
        logger.debug("indicator show() — dispatching whizFadeIn: to main thread")
        try:
            self._view.performSelectorOnMainThread_withObject_waitUntilDone_(
                "whizFadeIn:", None, False
            )
        except Exception as e:  # noqa: BLE001
            logger.warning("indicator show() dispatch failed: %s", e, exc_info=True)

    def hide(self) -> None:
        """Dismiss the overlay with an opacity fade (main-thread-safe)."""
        if self._view is None:
            logger.warning("indicator hide() but _view is None — panel not created")
            return
        self._visible = False
        logger.debug("indicator hide() — dispatching whizFadeOut: to main thread")
        try:
            self._view.performSelectorOnMainThread_withObject_waitUntilDone_(
                "whizFadeOut:", None, False
            )
        except Exception as e:  # noqa: BLE001
            logger.warning("indicator hide() dispatch failed: %s", e, exc_info=True)

    def update_level(self, level: float) -> None:
        """Feed a live mic amplitude in [0.0, 1.0] to animate the waveform.

        Thread-safe: dispatches the actual view update to the main thread.
        """
        self._level = max(0.0, min(1.0, level))
        if self._view is not None:
            try:
                self._view.performSelectorOnMainThread_withObject_waitUntilDone_(
                    "whizUpdateDisplay:", None, False
                )
            except Exception:  # noqa: BLE001
                pass

    def set_state(self, state: str) -> None:
        """Notify the indicator of a state change (listening/transcribing/idle)."""
        self._state = state
        if self._view is not None:
            try:
                self._view.performSelectorOnMainThread_withObject_waitUntilDone_(
                    "whizSetState:", None, False
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
        # CRITICAL for background agents: NSPanel defaults to
        # hidesOnDeactivate=True, so when whiz runs as a LaunchAgent
        # (accessory app, always "deactivated" — no Dock presence) the
        # panel hides itself and the indicator is invisible. Disabling
        # this keeps the overlay on screen regardless of activation state.
        self._panel.setHidesOnDeactivate_(False)
        # Start fully transparent — show() fades the alpha up.
        self._panel.setAlphaValue_(0.0)

        # Vibrancy view as the panel's contentView — this gives the native
        # macOS HUD blur (content behind the window is blurred). It's the
        # BACKGROUND layer; the custom draw view sits on top of it and
        # paints the logo + waveform.
        #
        # The previous approach added the vibrancy view as a subview of
        # the custom draw view inside drawRect_. But subviews render ON
        # TOP of their superview's drawn content, so the vibrancy
        # material covered the logo and waveform — the pill appeared
        # as just a blur blob with no visible glyph.
        #
        # Guard with getattr so test mocks (which don't include
        # NSVisualEffectView) and older macOS SDKs degrade to a plain
        # panel without the blur, rather than crashing.
        vfx = None
        NSVisualEffectView = getattr(AppKit, "NSVisualEffectView", None)
        if NSVisualEffectView is not None:
            try:
                vfx = NSVisualEffectView.alloc().initWithFrame_(frame)
                vfx.setMaterial_(AppKit.NSVisualEffectMaterialHUDWindow)
                vfx.setBlendingMode_(AppKit.NSVisualEffectBlendingModeBehindWindow)
                vfx.setState_(AppKit.NSVisualEffectStateActive)
                vfx.setWantsLayer_(True)
                try:
                    vfx.layer().setCornerRadius_(_PANEL_CORNER_RADIUS)
                    vfx.layer().setMasksToBounds_(True)
                except Exception:  # noqa: BLE001
                    pass
                self._panel.setContentView_(vfx)
            except Exception:  # noqa: BLE001
                logger.debug("vibrancy contentView setup failed", exc_info=True)
                vfx = None

        # Custom draw view on top of the vibrancy — transparent background
        # so the blur shows through, with the logo + waveform painted on.
        view_cls = _get_objc_view_class()
        self._view = view_cls.alloc().initWithFrame_(frame)
        self._view._indicator = self  # back-reference for state/level reads
        self._view._vfx_view = vfx   # keep strong ref so it isn't GC'd
        try:
            self._view.setWantsLayer_(True)
        except Exception:  # noqa: BLE001
            pass
        if vfx is not None:
            vfx.addSubview_(self._view)
        else:
            self._panel.setContentView_(self._view)


class WhizIndicatorView:
    """Pure-Python holder for the NSView draw method.

    The real ObjC ``_WhizIndicatorViewImpl`` subclass (created at import
    time on macOS via ``_create_objc_view_class``) delegates ``drawRect_``
    here. Keeping the draw logic in a plain Python class avoids requiring a
    compiled PyObjC subclass at import time and keeps this module importable
    without AppKit (the draw method is simply never called on non-macOS).
    """

    # Set by the ObjC subclass after alloc().initWithFrame_().
    _indicator: "MacIndicator"

    def drawRect_(self, rect) -> None:  # noqa: ARG002
        """NSView draw — paints the vibrancy pill + glyph + waveform bars."""
        try:
            import AppKit
            from Foundation import NSRect

            bounds = self.bounds()
            w = bounds.size.width
            h = bounds.size.height

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

            # Vibrancy background is the panel's contentView (set up in
            # _create_panel), not a subview added here. Adding it as a
            # subview during draw covered the logo/waveform because subviews
            # render on top of the superview's drawn content.

            # 1. A subtle rounded outline so the pill reads against light
            # backgrounds (the vibrancy alone can wash out on bright walls).
            bg_path = AppKit.NSBezierPath.bezierPathWithRoundedRect_xRadius_yRadius_(
                NSRect((0, 0), (w, h)), _PANEL_CORNER_RADIUS, _PANEL_CORNER_RADIUS
            )
            outline = AppKit.NSColor.colorWithCalibratedRed_green_blue_alpha_(
                1.0, 1.0, 1.0, 0.08
            )
            outline.set()
            bg_path.setLineWidth_(0.5)
            bg_path.stroke()

            # 3. whiz waveform-W logo on the left, tinted by state.
            from whiz.dictate.providers.macos_logo import draw_whiz_logo

            glyph_size = 20
            glyph_x = 14
            glyph_y = (h - glyph_size) / 2
            tint = AppKit.NSColor.colorWithCalibratedRed_green_blue_alpha_(*color)
            draw_whiz_logo(
                AppKit,
                NSRect((glyph_x, glyph_y), (glyph_size, glyph_size)),
                tint,
            )

            # 4. Waveform bars to the right of the glyph, heights tracking level.
            bars_x = glyph_x + glyph_size + 12
            cy = h / 2
            # Each bar's height is a slightly different function of level so
            # they don't all jump in unison — looks like a real waveform.
            for i in range(_BAR_COUNT):
                # Per-bar phase so adjacent bars differ.
                phase = (i - (_BAR_COUNT - 1) / 2) * 0.35
                amp = max(0.0, min(1.0, level + phase * 0.15))
                bar_h = _BAR_MIN_HEIGHT + amp * (_BAR_MAX_HEIGHT - _BAR_MIN_HEIGHT)
                bx = bars_x + i * (_BAR_WIDTH + _BAR_GAP)
                by = cy - bar_h / 2
                bar = AppKit.NSBezierPath.bezierPathWithRoundedRect_xRadius_yRadius_(
                    NSRect((bx, by), (_BAR_WIDTH, bar_h)), _BAR_CORNER, _BAR_CORNER
                )
                AppKit.NSColor.colorWithCalibratedRed_green_blue_alpha_(*color).set()
                bar.fill()
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
                self._vfx_view = None
                self._fade_gen = 0
                self._pending_out_gen = 0
            return self

        def drawRect_(self, rect):
            try:
                WhizIndicatorView.drawRect_(self, rect)
            except Exception:  # noqa: BLE001
                logger.debug("drawRect failed", exc_info=True)

        # performSelectorOnMainThread:withObject: requires a one-argument
        # selector (trailing colon in ObjC). pyobjc maps Python trailing
        # underscores to ObjC colons, but internal underscores also map to
        # colons — so we use CamelCase names (no internal underscores) to keep
        # the mapping unambiguous: whizUpdateDisplay_ -> whizUpdateDisplay:.
        def whizUpdateDisplay_(self, sender):  # noqa: ARG002
            try:
                self.setNeedsDisplay_(True)
            except Exception:  # noqa: BLE001
                logger.debug("whizUpdateDisplay failed", exc_info=True)

        def whizSetState_(self, sender):  # noqa: ARG002
            try:
                self.setNeedsDisplay_(True)
            except Exception:  # noqa: BLE001
                logger.debug("whizSetState failed", exc_info=True)

        # Panel fade in/out: drive the panel's alphaValue toward the target.
        # Using a short NSAnimationContext implicit animation eases the alpha.
        def whizFadeIn_(self, sender):  # noqa: ARG002
            self._fade_panel(True)

        def whizFadeOut_(self, sender):  # noqa: ARG002
            self._fade_panel(False)

        def _fade_panel(self, fade_in: bool) -> None:
            # Any exception raised inside an AppKit run-loop callback is
            # FATAL — it tears down the whole process (and under a KeepAlive
            # LaunchAgent, crash-loops). So the entire fade is wrapped in a
            # try/except with a non-animated fallback that still shows/hides
            # the panel. The indicator may pop instead of fading, but the
            # process survives and the hotkey + menu bar keep working.
            logger.debug("_fade_panel(fade_in=%s) called", fade_in)
            try:
                self._fade_panel_impl(fade_in)
                logger.debug("_fade_panel(fade_in=%s) impl succeeded", fade_in)
            except Exception as e:  # noqa: BLE001
                logger.warning("indicator fade failed; using instant fallback: %s", e, exc_info=True)
                try:
                    panel = self.window()
                    if panel is None:
                        logger.warning("_fade_panel fallback: panel is None")
                        return
                    if fade_in:
                        panel.orderFrontRegardless()
                        panel.setAlphaValue_(1.0)
                        logger.debug("_fade_panel fallback: ordered front + alpha 1.0")
                    else:
                        panel.setAlphaValue_(0.0)
                        panel.orderOut_(None)
                except Exception:  # noqa: BLE001
                    logger.warning("_fade_panel fallback also failed", exc_info=True)
                    pass

        def _fade_panel_impl(self, fade_in: bool) -> None:
            from AppKit import NSAnimationContext

            panel = self.window()
            if panel is None:
                return
            if fade_in:
                # Bump the generation token so any deferred orderOut from a
                # prior hide sees a stale gen and aborts (see whizOrderOut_:).
                self._fade_gen += 1
                # Order front FIRST (alpha is 0 from setup), then animate
                # alpha to 1. If the animation fails, set alpha instantly.
                # This guarantees the panel is on screen even if NSAnimationContext
                # is unavailable (headless test / no graphics context).
                panel.orderFrontRegardless()
                try:
                    ctx = NSAnimationContext.beginGrouping()
                    if ctx is not None:
                        ctx.setDuration_(_FADE_SECONDS)
                        panel.animator().setAlphaValue_(1.0)
                        NSAnimationContext.endGrouping()
                    else:
                        panel.setAlphaValue_(1.0)
                except Exception:  # noqa: BLE001
                    panel.setAlphaValue_(1.0)
            else:
                # Animate alpha to 0, then order out. If animation fails,
                # set alpha 0 instantly and order out immediately.
                self._fade_gen += 1
                gen = self._fade_gen
                try:
                    ctx = NSAnimationContext.beginGrouping()
                    if ctx is not None:
                        ctx.setDuration_(_FADE_SECONDS)
                        panel.animator().setAlphaValue_(0.0)
                        NSAnimationContext.endGrouping()
                        # Order out after the fade completes — but only if
                        # no show bumped the gen since (rapid hide→show).
                        # We store the gen in _pending_out_gen and pass nil to
                        # whizOrderOut: (ObjC performSelector needs an ObjC
                        # object, not a Python int). We can't cancel a
                        # scheduled perform in pyobjc
                        # (cancelPreviousPerformRequestsWithTarget: is not
                        # callable on instances), so we let stale callbacks
                        # fire and abort inside whizOrderOut:.
                        self._pending_out_gen = gen
                        self.performSelector_withObject_afterDelay_(
                            "whizOrderOut:", None, _FADE_SECONDS + 0.05
                        )
                    else:
                        panel.setAlphaValue_(0.0)
                        panel.orderOut_(None)
                except Exception:  # noqa: BLE001
                    panel.setAlphaValue_(0.0)
                    panel.orderOut_(None)

        def whizOrderOut_(self, sender):  # noqa: ARG002
            """Deferred hide: order out the panel unless superseded by a show.

            ``_pending_out_gen`` was snapshotted at hide time. If a show has
            bumped ``_fade_gen`` since, the panel is visible again and we
            skip the orderOut. This replaces
            ``cancelPreviousPerformRequestsWithTarget:`` which is not
            callable on pyobjc NSPanel instances (AttributeError).
            """
            try:
                if getattr(self, "_pending_out_gen", 0) != self._fade_gen:
                    return  # superseded by a show — don't yank the panel
                panel = self.window()
                if panel is not None:
                    panel.orderOut_(None)
            except Exception:  # noqa: BLE001
                logger.debug("whizOrderOut failed", exc_info=True)

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