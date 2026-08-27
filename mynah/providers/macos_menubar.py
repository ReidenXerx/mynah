"""MacMenuBar — a menu bar status item for dictation control (macOS).

An ``NSStatusItem`` in the system menu bar with a custom-drawn whiz
waveform-W logo whose tint reflects the current state (gray idle / cyan
listening / amber transcribing). A click opens an ``NSMenu`` with:

- **Start Dictation / Stop Dictation** — toggles the session (same path as the
  hotkey) via ``engine.toggle_session()``.
- a disabled state line showing the current status.
- **Open Config File** — reveals ``~/.config/whiz/config.toml`` in Finder /
  opens it in the default editor via ``NSWorkspace``.
- **About whiz** — version, model, hotkey.
- **Quit whiz dictate** — ``engine.stop()`` (the process exits; under a
  KeepAlive LaunchAgent launchd restarts it).

The menu bar item lives inside the same LaunchAgent process as the engine
and indicator, so it drives the engine directly — no IPC. The engine
registers ``MacMenuBar.on_state`` as a state listener so the icon + menu
labels track the indicator without the menu bar knowing about the indicator.

Requires pyobjc/AppKit; on non-macOS or without pyobjc the engine's
``_setup_menu_bar`` catches the ImportError and degrades to no menu bar.
"""

from __future__ import annotations

import logging
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:  # pragma: no cover - typing only
    from whiz.dictate.engine import DictationEngine

logger = logging.getLogger(__name__)

# State → tint color (RGBA floats 0–1). The logo is monochrome and
# stroked in this color per state (gray idle / cyan listening / amber
# transcribing).
_STATE_COLOR = {
    "idle": (0.6, 0.6, 0.65, 1.0),       # gray
    "listening": (0.2, 0.8, 0.95, 1.0),  # cyan
    "transcribing": (0.95, 0.7, 0.2, 1.0),  # amber
}
# Menu bar status item icon size (points). Menu bar items render at ~16-18px;
# 18 gives the W enough room for its round-cap strokes to read clearly.
_MENU_ICON_SIZE = 18.0


class MacMenuBar:
    """A macOS menu bar status item that controls the dictation engine."""

    def __init__(self, engine: "DictationEngine") -> None:
        self._engine = engine
        self._status_item: Any = None
        self._button: Any = None
        self._menu: Any = None
        self._controller: Any = None  # the ObjC controller (holds strong refs)
        self._toggle_item: Any = None
        self._state_item: Any = None
        self._state: str = "idle"

    def setup(self) -> None:
        """Create the status item + menu on the main thread (idempotent)."""
        if self._status_item is not None:
            return
        try:
            self._create_status_item()
        except ImportError:
            logger.warning(
                "PyObjC not available — dictation menu bar item disabled. "
                "Install: pipx inject whiz 'whiz[dictate]'"
            )
        except Exception:  # noqa: BLE001
            logger.warning("Could not create dictation menu bar item", exc_info=True)

    def on_state(self, state: str) -> None:
        """State-listener callback: update icon tint + menu labels.

        Called inline from the engine's ``_set_state`` on whatever thread
        initiated the change. AppKit UI mutations must run on the main
        thread, so dispatch the actual update there.

        ``whizUpdateMenuState:`` is defined on ``_WhizMenuBarController``
        (the ObjC NSObject), NOT on ``NSStatusBarButton`` — so dispatch to
        ``self._controller``, not ``self._button``. Dispatching to the
        button raises ``NSInvalidArgumentException`` (unrecognized selector)
        and crashes the process on the first state change.
        """
        self._state = state
        if self._controller is None:
            return
        try:
            self._controller.performSelectorOnMainThread_withObject_waitUntilDone_(
                "whizUpdateMenuState:", None, False
            )
        except Exception:  # noqa: BLE001
            pass

    # ---------- setup ----------

    def _create_status_item(self) -> None:
        """Build the NSStatusItem + NSMenu + ObjC controller. Imports pyobjc lazily."""
        import AppKit

        controller_cls = _get_objc_controller_class()
        self._controller = controller_cls.alloc().init()
        self._controller._menubar = self  # back-reference for action dispatch

        bar = AppKit.NSStatusBar.systemStatusBar()
        self._status_item = bar.statusItemWithLength_(
            AppKit.NSVariableStatusItemLength
        )
        self._button = self._status_item.button()
        self._button.setTarget_(self._controller)
        self._button.setAction_("whizMenuClicked:")
        # Apply the initial (idle) icon.
        self._apply_icon("idle")

        # Build the menu.
        menu = AppKit.NSMenu.alloc().init()
        menu.setAutoenablesItems_(False)

        # Start/Stop toggle.
        self._toggle_item = AppKit.NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(
            "Start Dictation", "whizToggle:", ""
        )
        self._toggle_item.setTarget_(self._controller)
        self._toggle_item.setEnabled_(True)
        menu.addItem_(self._toggle_item)

        # Disabled state line.
        self._state_item = AppKit.NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(
            "○ Idle", None, ""
        )
        self._state_item.setEnabled_(False)
        menu.addItem_(self._state_item)

        menu.addItem_(AppKit.NSMenuItem.separatorItem())

        # Open Config File.
        open_item = AppKit.NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(
            "Open Config File", "whizOpenConfig:", ""
        )
        open_item.setTarget_(self._controller)
        open_item.setEnabled_(True)
        menu.addItem_(open_item)

        menu.addItem_(AppKit.NSMenuItem.separatorItem())

        # About.
        about_item = AppKit.NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(
            "About whiz", "whizAbout:", ""
        )
        about_item.setTarget_(self._controller)
        about_item.setEnabled_(True)
        menu.addItem_(about_item)

        # Quit.
        quit_item = AppKit.NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(
            "Quit whiz dictate", "whizQuit:", "q"
        )
        quit_item.setTarget_(self._controller)
        quit_item.setEnabled_(True)
        menu.addItem_(quit_item)

        self._menu = menu
        self._status_item.setMenu_(menu)

        # Initial label sync.
        self._update_labels()

    # ---------- state application (main thread) ----------

    def _apply_icon(self, state: str) -> None:
        """Set the whiz waveform-W logo tinted for ``state``.

        Renders the custom W monogram (not SF Symbols) as a monochrome
        NSImage and sets it on the menu bar button. Our state colors
        (cyan/amber) carry meaning, so we render them in directly and keep
        ``template`` off; the W is always the brand mark, just colored by
        activity state.
        """
        try:
            import AppKit

            from whiz.dictate.providers.macos_logo import whiz_logo_image

            color_rgba = _STATE_COLOR.get(state, _STATE_COLOR["idle"])
            color = AppKit.NSColor.colorWithCalibratedRed_green_blue_alpha_(*color_rgba)
            img = whiz_logo_image(_MENU_ICON_SIZE, color)
            if img is not None:
                self._button.setImage_(img)
        except Exception:  # noqa: BLE001
            logger.debug("menu bar icon apply failed", exc_info=True)

    def _update_labels(self) -> None:
        """Sync the toggle + state-line labels with the current state."""
        state = self._state
        active = state in ("listening", "transcribing")
        if self._toggle_item is not None:
            self._toggle_item.setTitle_(
                "Stop Dictation" if active else "Start Dictation"
            )
        if self._state_item is not None:
            label = {
                "listening": "● Listening",
                "transcribing": "● Transcribing…",
            }.get(state, "○ Idle")
            self._state_item.setTitle_(label)
        # Re-apply the icon so the tint tracks the state.
        self._apply_icon(state)

    # ---------- menu actions (called from the ObjC controller) ----------

    def do_toggle(self) -> None:
        """Start/Stop Dictation menu action → engine.toggle_session().

        Dispatched to a background thread so the ``whizToggle:`` selector
        returns immediately and the AppKit run loop stays responsive. The
        first session loads the STT model (a multi-second mlx-whisper
        download/load); running that synchronously on the main thread would
        freeze the menu bar icon, the menu, and the indicator's fade
        animation for the whole load. The hotkey path already runs
        off-main (pynput listener thread), so this mirrors it.
        """
        import threading

        threading.Thread(
            target=self._engine.toggle_session, daemon=True
        ).start()

    def do_open_config(self) -> None:
        """Open Config File → reveal ~/.config/whiz/config.toml in Finder.

        Runs off the main thread (dispatched by ``do_open_config`` via a
        worker) so the ``whizOpenConfig:`` selector returns immediately and
        the AppKit run loop isn't blocked. The LaunchAgent runs as an
        accessory (background) app, so ``activateFileViewerSelecting:`` alone
        can fail silently — Finder needs an explicit activation nudge first.
        If the config file doesn't exist yet (fresh install, never saved),
        we create it with current defaults so the user always sees something.
        """
        import subprocess
        import threading

        def _open() -> None:
            try:
                from whiz.config import CONFIG_PATH, Config, save

                path = CONFIG_PATH
                # Ensure the file exists — a fresh install may never have
                # written it, and activateFileViewerSelecting: on a missing
                # path silently does nothing (the menu item appears dead).
                if not path.exists():
                    try:
                        save(Config())
                    except Exception:  # noqa: BLE001
                        logger.debug("could not create config file", exc_info=True)

                # `open` launches the file in the user's default app for .toml
                # (TextEdit, VS Code, etc.) and brings it to the foreground.
                # This is more useful than a Finder reveal for a config file
                # the user wants to *edit*, and it works from a background app.
                subprocess.run(
                    ["/usr/bin/open", str(path)],
                    check=False,
                    timeout=5,
                )
            except Exception:  # noqa: BLE001
                logger.debug("open config failed", exc_info=True)

        threading.Thread(target=_open, daemon=True).start()

    def do_about(self) -> None:
        """About whiz → print version/model/hotkey to stderr (no modal in a bg app)."""
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

    def do_quit(self) -> None:
        """Quit whiz dictate → engine.stop() off-main so the selector returns
        immediately and the AppKit run loop isn't blocked.

        ``engine.stop()`` joins the capture (5s) and transcribe (30s) threads;
        running it on the main thread inside ``whizQuit:`` would beach-ball
        the menu bar for up to ~35s on an active session. Under a KeepAlive
        LaunchAgent launchd restarts the process anyway, so a fast stop is
        preferable to a frozen clean shutdown.
        """
        import threading

        threading.Thread(
            target=self._engine.stop, daemon=True
        ).start()


# ---------- ObjC runtime glue ----------
# A real NSObject subclass receives the menu action callbacks (target/action)
# and the button click, and dispatches them back to the Python MacMenuBar.
# Created once at import time on macOS; on non-macOS it stays None and
# _get_objc_controller_class raises ImportError (caught by setup()).

_OBJC_CONTROLLER_CLASS = None


def _create_objc_controller_class():
    """Create (once) an NSObject subclass that dispatches menu actions to MacMenuBar."""
    global _OBJC_CONTROLLER_CLASS
    if _OBJC_CONTROLLER_CLASS is not None:
        return _OBJC_CONTROLLER_CLASS
    import objc
    from Foundation import NSObject

    class _WhizMenuBarController(NSObject):
        def init(self):
            self = objc.super(_WhizMenuBarController, self).init()
            if self is not None:
                self._menubar = None
            return self

        # Menu bar icon click → show the menu (NSStatusItem.menu handles this
        # automatically, but keep an action so the button isn't a no-op).
        def whizMenuClicked_(self, sender):  # noqa: ARG002
            try:
                pass
            except Exception:  # noqa: BLE001
                logger.debug("whizMenuClicked failed", exc_info=True)

        def whizToggle_(self, sender):  # noqa: ARG002
            try:
                mb = getattr(self, "_menubar", None)
                if mb is not None:
                    mb.do_toggle()
            except Exception:  # noqa: BLE001
                logger.debug("whizToggle failed", exc_info=True)

        def whizOpenConfig_(self, sender):  # noqa: ARG002
            try:
                mb = getattr(self, "_menubar", None)
                if mb is not None:
                    mb.do_open_config()
            except Exception:  # noqa: BLE001
                logger.debug("whizOpenConfig failed", exc_info=True)

        def whizAbout_(self, sender):  # noqa: ARG002
            try:
                mb = getattr(self, "_menubar", None)
                if mb is not None:
                    mb.do_about()
            except Exception:  # noqa: BLE001
                logger.debug("whizAbout failed", exc_info=True)

        def whizQuit_(self, sender):  # noqa: ARG002
            try:
                mb = getattr(self, "_menubar", None)
                if mb is not None:
                    mb.do_quit()
            except Exception:  # noqa: BLE001
                logger.debug("whizQuit failed", exc_info=True)

        # Called via performSelectorOnMainThread from on_state to update the
        # icon tint + menu labels on the main thread.
        def whizUpdateMenuState_(self, sender):  # noqa: ARG002
            try:
                mb = getattr(self, "_menubar", None)
                if mb is not None:
                    mb._update_labels()
            except Exception:  # noqa: BLE001
                logger.debug("whizUpdateMenuState failed", exc_info=True)

    _OBJC_CONTROLLER_CLASS = _WhizMenuBarController
    return _OBJC_CONTROLLER_CLASS


def _get_objc_controller_class():
    """Return the ObjC NSObject controller subclass (creates it once).

    Raises ImportError on non-macOS or when pyobjc/Foundation is unavailable.
    """
    cls = _create_objc_controller_class()
    if cls is None:
        raise ImportError("ObjC menu bar controller unavailable")
    return cls


# Eagerly create the ObjC subclass at import time on macOS so .alloc().init()
# is available before setup() runs. On non-macOS the import fails and the class
# stays None; _get_objc_controller_class() raises ImportError.
try:
    _create_objc_controller_class()
except ImportError:
    pass
except Exception:  # noqa: BLE001 - never fail import on a pyobjc quirk
    logger.debug("menu bar ObjC controller class not created", exc_info=True)