"""Typing into apps that ignore a virtual keyboard: the clipboard.

`wtype` works by uploading its own keymap over the virtual-keyboard protocol
and then sending keycodes from it. An app that does not apply that keymap reads
those keycodes with whatever layout the real keyboard has — and because the
generated keymap assigns characters to keycodes from 1 upward, "echo plain
abcdef" arrives as `1234567894701-=`. Warp does exactly this. So does anything
else that treats the seat's first keyboard as the only one.

Nothing in the protocol lets us detect that; the app just renders digits. What
does work everywhere is the clipboard: one chord, arbitrary Unicode, no keymap
involved. So this injector puts the utterance on the clipboard, sends the paste
chord through the compositor rather than through a virtual keyboard, and puts
the clipboard back the way it was.

Costs, stated plainly: the clipboard is borrowed for about a second per
utterance, a clipboard holding something that is not text cannot be restored,
and the target app must have a paste shortcut. That is why this is not the
default — see `SmartInjector`, which uses it only where typing is known to
fail.
"""

from __future__ import annotations

import json
import logging
import os
import shutil
import subprocess
import threading
import time

from mynah import control
from mynah.providers.base import TextInjector
from mynah.providers.linux_inject import WtypeInjector, is_wayland

logger = logging.getLogger("mynah.inject.clipboard")

# How long to leave our text on the clipboard before putting back what was
# there. The paste is synchronous from the compositor's side, but the app reads
# the selection asynchronously, and reading a clipboard that has already been
# replaced yields nothing.
RESTORE_AFTER = 0.8

# Terminals paste with Ctrl+Shift+V, because Ctrl+V is a control character to
# the program inside them. Matched as substrings of the window class, lowered.
TERMINAL_CLASSES = (
    "warp", "foot", "kitty", "alacritty", "wezterm", "ghostty", "konsole",
    "gnome-terminal", "xterm", "urxvt", "terminator", "tilix", "blackbox",
    "org.wezfurlong", "com.mitchellh.ghostty", "st-256color",
)


def _tool(name: str) -> str | None:
    return shutil.which(name)


def focused_class() -> str:
    """The class of the focused window, lowercased, or "" if we cannot ask."""
    if not os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
        return ""
    hyprctl = _tool("hyprctl")
    if hyprctl is None:
        return ""
    try:
        done = subprocess.run(
            [hyprctl, "-j", "activewindow"], capture_output=True, text=True, timeout=2, check=False
        )
        if done.returncode != 0:
            return ""
        window = json.loads(done.stdout)
    except (OSError, ValueError, subprocess.SubprocessError):
        return ""
    if not isinstance(window, dict):
        return ""
    return str(window.get("class", "")).lower()


def paste_chord(window_class: str) -> tuple[str, str]:
    """The paste shortcut for this window, as (mods, key)."""
    if any(name in window_class for name in TERMINAL_CLASSES):
        return "CTRL SHIFT", "V"
    return "CTRL", "V"


class ClipboardInjector(TextInjector):
    """Pastes the utterance instead of typing it."""

    name = "clipboard"

    def __init__(self) -> None:
        self._copy = _tool("wl-copy")
        self._paste = _tool("wl-paste")
        self._hyprctl = _tool("hyprctl")
        self._fallback = WtypeInjector()
        self._restore_timer: threading.Timer | None = None

    # ---------- the clipboard ----------

    def _read_clipboard(self) -> str | None:
        """What is on the clipboard, if it is text we can put back.

        None means "do not try to restore": either nothing is there, or it is
        an image or a file list, which this cannot carry.
        """
        if self._paste is None:
            return None
        try:
            types = subprocess.run(
                [self._paste, "--list-types"], capture_output=True, text=True, timeout=2, check=False
            )
            if types.returncode != 0 or "text/plain" not in types.stdout:
                return None
            done = subprocess.run(
                [self._paste, "--no-newline", "--type", "text/plain"],
                capture_output=True, text=True, timeout=2, check=False,
            )
        except (OSError, subprocess.SubprocessError):
            return None
        return done.stdout if done.returncode == 0 else None

    def _write_clipboard(self, text: str) -> bool:
        """Put the text on the clipboard.

        wl-copy forks a daemon that serves the selection until something
        replaces it, and that daemon inherits the pipes. Capturing its output
        therefore waits for a process designed not to exit — the first version
        of this timed out on every utterance and fell back to typing. So its
        output goes to /dev/null and only the parent is waited for.
        """
        if self._copy is None:
            return False
        try:
            done = subprocess.run(
                [self._copy, "--type", "text/plain", "--"],
                input=text, text=True, timeout=5, check=False,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
        except (OSError, subprocess.SubprocessError):
            logger.debug("wl-copy failed", exc_info=True)
            return False
        return done.returncode == 0

    def _restore_clipboard(self, saved: str | None) -> None:
        if saved is None or self._copy is None:
            return
        try:
            subprocess.run(
                [self._copy, "--type", "text/plain", "--"],
                input=saved, text=True, timeout=5, check=False,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
        except (OSError, subprocess.SubprocessError):
            logger.debug("could not restore the clipboard", exc_info=True)

    # ---------- the chord ----------

    def _send_chord(self, mods: str, key: str) -> bool:
        """Ask the compositor to deliver the shortcut.

        Through Hyprland rather than through a virtual keyboard: the whole
        reason we are here is an app that ignores virtual keyboards.
        """
        if self._hyprctl is not None and os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
            lua = (
                'hl.dsp.send_shortcut({ mods = "%s", key = "%s", window = "activewindow" })'
                % (mods, key)
            )
            try:
                done = subprocess.run(
                    [self._hyprctl, "dispatch", lua],
                    capture_output=True, text=True, timeout=3, check=False,
                )
            except (OSError, subprocess.SubprocessError):
                return False
            if done.returncode == 0 and "error" not in (done.stdout or "").lower():
                return True
            logger.debug("send_shortcut refused: %s", (done.stdout or done.stderr).strip())
        # Elsewhere, the virtual keyboard is all there is. It works in every
        # app that does not have this problem in the first place.
        return self._fallback.send_chord(mods, key)

    # ---------- the interface ----------

    def type_text(self, text: str) -> None:
        if not text:
            return
        saved = self._read_clipboard()
        if not self._write_clipboard(text):
            logger.error("could not reach the clipboard; falling back to typing")
            self._fallback.type_text(text)
            return
        mods, key = paste_chord(focused_class())
        if not self._send_chord(mods, key):
            logger.error("could not send %s+%s; the text is on the clipboard", mods, key)
            self._restore_later(saved)
            return
        self._restore_later(saved)
        control.publish({"event": "text", "text": text})

    def _restore_later(self, saved: str | None) -> None:
        if saved is None:
            return
        if self._restore_timer is not None:
            self._restore_timer.cancel()
        self._restore_timer = threading.Timer(RESTORE_AFTER, self._restore_clipboard, args=(saved,))
        self._restore_timer.daemon = True
        self._restore_timer.start()

    def check_permissions(self, prompt: bool = True) -> tuple[bool, str]:
        if not is_wayland():
            return False, (
                "Not a Wayland session. mynah does not ship an X11 path — "
                "see docs/LINUX-APP.md."
            )
        missing = [name for name, found in (("wl-copy", self._copy), ("wl-paste", self._paste)) if found is None]
        if missing:
            return False, (
                f"{' and '.join(missing)} not installed — the clipboard injector needs them.\n"
                "  Arch:   sudo pacman -S wl-clipboard\n"
                "  Debian: sudo apt install wl-clipboard"
            )
        return True, ""


class SmartInjector(TextInjector):
    """Types with `wtype`, and pastes into the apps where typing does not work.

    The list below is not a guess: each entry is an app observed turning typed
    text into digits, which is what a virtual keymap being ignored looks like.
    Everything else types, because typing needs no clipboard and no shortcut.
    """

    name = "smart"

    # Matched as substrings of the lowercased window class.
    KEYMAP_DEAF = ("warp",)

    def __init__(self) -> None:
        self._typing = WtypeInjector()
        self._pasting = ClipboardInjector()
        # Cache the class between utterances of one burst: focus rarely moves
        # mid-sentence, and this is a subprocess.
        self._last_class = ""
        self._last_looked_at = 0.0

    def _class_now(self) -> str:
        now = time.monotonic()
        if now - self._last_looked_at < 0.5:
            return self._last_class
        self._last_class = focused_class()
        self._last_looked_at = now
        return self._last_class

    def type_text(self, text: str) -> None:
        window_class = self._class_now()
        if any(name in window_class for name in self.KEYMAP_DEAF):
            logger.debug("%s ignores virtual keymaps; pasting instead", window_class)
            self._pasting.type_text(text)
            return
        self._typing.type_text(text)

    def check_permissions(self, prompt: bool = True) -> tuple[bool, str]:
        # Typing is the normal path, so its requirements are the ones to report.
        return self._typing.check_permissions(prompt=prompt)


def _clipboard_ready() -> bool:
    """Whether wl-clipboard is installed — the availability probe."""
    return _tool("wl-copy") is not None and _tool("wl-paste") is not None
