"""Typing into the focused window on Wayland, with `wtype`.

`wtype` speaks the virtual-keyboard protocol, which is the compositor-blessed
way to synthesize input: the compositor decides whether to honour it, and the
user can see and revoke that in the compositor's own terms. X11 is not
supported here — see docs/LINUX-APP.md for why we do not ship a path where
every client can already do this to every other client.

The text goes in on **stdin**, never in argv. Argv is world-readable through
/proc, so passing a transcript as an argument would publish every dictated
sentence to every process on the machine for as long as the command ran.
"""

from __future__ import annotations

import logging
import os
import shutil
import subprocess

from mynah import control
from mynah.providers.base import TextInjector

logger = logging.getLogger("mynah.inject.wtype")

# A wedged `wtype` must not wedge dictation. Typing a long utterance is still
# well under a second.
TYPE_TIMEOUT = 10.0


def find_binary() -> str | None:
    override = os.environ.get("MYNAH_WTYPE")
    if override:
        return override if os.path.exists(override) else None
    return shutil.which("wtype")


def is_wayland() -> bool:
    return bool(os.environ.get("WAYLAND_DISPLAY")) or os.environ.get("XDG_SESSION_TYPE") == "wayland"


class WtypeInjector(TextInjector):
    """Types transcribed text into whatever window the compositor has focused."""

    name = "wtype"

    def __init__(self, binary: str | None = None) -> None:
        self._binary = binary or find_binary()

    def type_text(self, text: str) -> None:
        if not text:
            return
        if self._binary is None:
            logger.error("wtype is not installed — nothing was typed")
            return
        try:
            done = subprocess.run(
                [self._binary, "-"],
                input=text,
                text=True,
                capture_output=True,
                timeout=TYPE_TIMEOUT,
            )
        except subprocess.TimeoutExpired:
            logger.error("wtype timed out; the utterance was not typed")
            return
        if done.returncode != 0:
            logger.error("wtype exited %s: %s", done.returncode, (done.stderr or "").strip())
            return
        # The shell plugin shows what was typed; publishing here rather than in
        # the engine means the event fires exactly when the text really landed.
        control.publish({"event": "text", "text": text})

    def check_permissions(self, prompt: bool = True) -> tuple[bool, str]:
        """Wayland has no permission to ask for — only tools to have.

        The virtual-keyboard protocol is granted by the compositor at connect
        time, so there is no dialog and nothing to poll. What can be missing is
        `wtype` itself, or a session that is not Wayland at all.
        """
        if not is_wayland():
            return False, (
                "Not a Wayland session (XDG_SESSION_TYPE="
                f"{os.environ.get('XDG_SESSION_TYPE', 'unset')!r}).\n"
                "mynah types through the Wayland virtual-keyboard protocol and "
                "does not ship an X11 path — see docs/LINUX-APP.md."
            )
        if self._binary is None:
            return False, (
                "wtype is not installed — mynah cannot type what it hears.\n"
                "  Arch:   sudo pacman -S wtype\n"
                "  Debian: sudo apt install wtype"
            )
        return True, ""
