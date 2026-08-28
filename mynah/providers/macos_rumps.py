"""MacMenuBar — a rumps-based menu bar status item for dictation control (macOS).

Uses the ``rumps`` library (Ridiculously Uncomplicated macOS Python Statusbar
apps) which handles NSApplication lifecycle, NSStatusItem, NSMenu, and the
AppKit event loop internally — eliminating the fragile manual PyObjC glue
that caused the pill to never render, the dropdown to die after one use, and
the W icon to never change color.

The menu bar W logo tint reflects the current state (gray idle / cyan
listening / amber transcribing). A click opens an NSMenu with:

- **Start Dictation / Stop Dictation** — toggles the session.
- a disabled state line showing the current status.
- **Open Config File** — opens ``~/.config/whiz/config.toml`` in the default
  editor.
- **About whiz** — version, model, hotkey.
- **Quit whiz dictate** — ``engine.stop()``.

State icon changes use pre-rendered PNG files (generated at import time via
the existing ``draw_whiz_logo`` NSBezierPath drawing) so rumps can swap them
via ``app.icon = path`` — no manual NSImage lifecycle to manage.
"""

from __future__ import annotations

import logging
import os
import tempfile
import threading
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:  # pragma: no cover - typing only
    from whiz.dictate.engine import DictationEngine

logger = logging.getLogger(__name__)

# State → which pre-rendered PNG to use for the menu bar icon.
# The PNGs are generated on first import via _ensure_icon_pngs().
_STATE_ICON_KEY = {
    "idle": "idle",
    "listening": "listening",
    "transcribing": "transcribing",
}

# Icon size in points (menu bar renders at ~16-18px; 36 for retina @2x).
_ICON_SIZE = 36

# Colors (RGBA floats 0–1) matching the indicator colors.
_STATE_COLORS: dict[str, tuple[float, float, float, float]] = {
    "idle": (0.6, 0.6, 0.65, 1.0),       # gray
    "listening": (0.2, 0.8, 0.95, 1.0),  # cyan
    "transcribing": (0.95, 0.7, 0.2, 1.0),  # amber
}

# Cached PNG paths once generated.
_ICON_PNGS: dict[str, str] = {}


def _ensure_icon_pngs() -> dict[str, str]:
    """Generate whiz W logo PNGs for each state, return paths.

    Uses the existing ``draw_whiz_logo`` NSBezierPath drawing to render
    the W into an NSImage, then saves as PNG. Called once; cached in
    ``_ICON_PNGS``. On non-macOS or if pyobjc is unavailable, returns
    empty dict (rumps won't show an icon, just text).
    """
    if _ICON_PNGS:
        return _ICON_PNGS
    try:
        import AppKit
        from Foundation import NSRect, NSSize, NSPoint
        from whiz.dictate.providers.macos_logo import draw_whiz_logo
    except ImportError:
        logger.debug("icon PNG generation skipped (no pyobjc)")
        return _ICON_PNGS

    tmpdir = os.path.join(tempfile.gettempdir(), "whiz-icons")
    try:
        os.makedirs(tmpdir, exist_ok=True)
    except OSError:
        return _ICON_PNGS

    for state, color in _STATE_COLORS.items():
        path = os.path.join(tmpdir, f"whiz-w-{state}.png")
        try:
            size = _ICON_SIZE
            img = AppKit.NSImage.alloc().initWithSize_(NSSize(size, size))
            img.lockFocus()
            try:
                tint = AppKit.NSColor.colorWithCalibratedRed_green_blue_alpha_(*color)
                draw_whiz_logo(AppKit, NSRect((0, 0), (size, size)), tint)
            finally:
                img.unlockFocus()
            # Save as PNG via NSBitmapImageRep (representationUsingType_properties_
            # is on NSBitmapImageRep, not NSImage).
            tiff = img.TIFFRepresentation()
            if tiff is not None:
                rep = AppKit.NSBitmapImageRep.imageRepWithData_(tiff)
                if rep is not None:
                    png_data = rep.representationUsingType_properties_(
                        AppKit.NSImageTypePNG, None
                    )
                    if png_data is not None:
                        png_data.writeToFile_atomically_(path, True)
                        _ICON_PNGS[state] = path
                        logger.debug("generated icon PNG: %s", path)
        except Exception:  # noqa: BLE001
            logger.debug("icon PNG generation failed for state=%s", state, exc_info=True)

    return _ICON_PNGS


class MacMenuBar:
    """A macOS menu bar status item (via rumps) that controls the dictation engine."""

    def __init__(self, engine: "DictationEngine") -> None:
        self._engine = engine
        self._app: Any = None
        self._state: str = "idle"
        self._icons: dict[str, str] = {}
        self._started = False

    def setup(self) -> None:
        """Create the rumps App (idempotent). Does NOT run it yet — the
        engine calls run() after starting the hotkey listener.
        """
        if self._app is not None:
            return
        try:
            import rumps
        except ImportError:
            logger.warning(
                "rumps not available — dictation menu bar item disabled. "
                "Install: pipx inject whiz 'whiz[dictate]'"
            )
            return
        try:
            self._icons = _ensure_icon_pngs()
            app = rumps.App(
                name="whiz dictate",
                title="",
                icon=self._icons.get("idle"),
                quit_button=None,  # we add our own quit
            )
            self._app = app
            self._build_menu()
            logger.debug("rumps menu bar app created")
        except Exception:  # noqa: BLE001
            logger.warning("Could not create dictation menu bar item", exc_info=True)

    def _build_menu(self) -> None:
        """Build the NSMenu via rumps MenuItem API."""
        import rumps

        self._toggle_item = rumps.MenuItem("Start Dictation", callback=self._on_toggle)
        self._state_item = rumps.MenuItem("○ Idle", callback=None)
        self._state_item.set_callback(None)  # disabled
        self._open_config_item = rumps.MenuItem("Open Config File", callback=self._on_open_config)
        self._about_item = rumps.MenuItem("About whiz", callback=self._on_about)
        self._quit_item = rumps.MenuItem("Quit whiz dictate", callback=self._on_quit)

        self._app.menu = [
            self._toggle_item,
            self._state_item,
            None,  # separator
            self._open_config_item,
            None,  # separator
            self._about_item,
            self._quit_item,
        ]

    def run(self) -> None:
        """Run the rumps App event loop (blocks the calling thread).

        Called from the engine's _run_with_appkit on the main thread.
        rumps.App.run() handles NSApplication.sharedApplication(),
        setActivationPolicy, the NSStatusItem, and the AppKit run loop.
        """
        if self._app is None:
            return
        self._started = True
        try:
            self._app.run()
        except Exception:  # noqa: BLE001
            logger.warning("rumps app run failed", exc_info=True)

    def on_state(self, state: str) -> None:
        """State-listener callback: update icon tint + menu labels.

        Called from the engine's ``_set_state`` on whatever thread initiated
        the change. rumps properties can be set from any thread — rumps
        dispatches UI updates to the main thread internally.
        """
        self._state = state
        if self._app is None:
            return
        try:
            active = state in ("listening", "transcribing")
            self._toggle_item.title = "Stop Dictation" if active else "Start Dictation"
            label = {
                "listening": "● Listening",
                "transcribing": "● Transcribing…",
            }.get(state, "○ Idle")
            self._state_item.title = label
            # Swap icon.
            icon_key = _STATE_ICON_KEY.get(state, "idle")
            icon_path = self._icons.get(icon_key)
            if icon_path:
                self._app.icon = icon_path
            logger.debug("on_state: state=%s icon=%s", state, icon_path)
        except Exception:  # noqa: BLE001
            logger.debug("on_state update failed", exc_info=True)

    # ---------- menu actions ----------

    def do_toggle(self) -> None:
        """Public toggle entry point — same as the menu callback.

        Used by tests and as a stable API; the menu callback ``_on_toggle``
        delegates here.
        """
        threading.Thread(target=self._engine.toggle_session, daemon=True).start()

    def do_quit(self) -> None:
        """Public quit entry point — stop the engine then quit rumps."""
        import rumps

        def _quit() -> None:
            try:
                self._engine.stop()
            except Exception:  # noqa: BLE001
                pass
            try:
                rumps.quit_application()
            except Exception:  # noqa: BLE001
                pass

        threading.Thread(target=_quit, daemon=True).start()

    def _on_toggle(self, sender):  # noqa: ARG002
        """Start/Stop Dictation menu callback → do_toggle."""
        self.do_toggle()

    def _on_open_config(self, sender):  # noqa: ARG002
        """Open Config File → open ~/.config/whiz/config.toml in default editor."""
        import subprocess

        def _open() -> None:
            try:
                from whiz.config import CONFIG_PATH, Config, save

                path = CONFIG_PATH
                if not path.exists():
                    try:
                        save(Config())
                    except Exception:  # noqa: BLE001
                        logger.debug("could not create config file", exc_info=True)
                subprocess.run(["/usr/bin/open", str(path)], check=False, timeout=5)
            except Exception:  # noqa: BLE001
                logger.debug("open config failed", exc_info=True)

        threading.Thread(target=_open, daemon=True).start()

    def _on_about(self, sender):  # noqa: ARG002
        """About whiz → print version/model/hotkey to stderr."""
        try:
            from whiz import __version__

            engine = self._engine
            model = getattr(engine.stt, "_model_ref", "?")
            print(
                f"whiz {__version__} — dictate\n"
                f"  model:  {model}\n"
                f"  hotkey: {engine.s.hotkey}\n"
                f"  mode:   {engine.s.trigger}",
                file=__import__("sys").stderr,
            )
        except Exception:  # noqa: BLE001
            logger.debug("about failed", exc_info=True)

    def _on_quit(self, sender):  # noqa: ARG002
        """Quit whiz dictate menu callback → do_quit."""
        self.do_quit()
