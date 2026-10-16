"""Typing on KDE Plasma, where no virtual keyboard is offered.

KWin gives ordinary programs no virtual-keyboard protocol, so `wtype` cannot work there at all. What Plasma does
offer is the RemoteDesktop portal: a program may press keys once the person has allowed it (Plasma asks the first
time and remembers). So the utterance goes onto the clipboard through Klipper, the paste chord is pressed through
the portal -- Ctrl+Shift+V when KWin says a terminal is in front -- and the clipboard is put back.

The work is done by `plasma_type.py` beside this file, run with the system Python, because it needs PyGObject for
D-Bus and this engine's own environment does not have it. The text travels on its stdin, never in arguments.
"""

from __future__ import annotations

import glob
import logging
import os
import pathlib
import subprocess

from mynah import control
from mynah.providers.base import TextInjector

logger = logging.getLogger("mynah.inject.plasma")

SYSTEM_PYTHON = "/usr/bin/python3"
SCRIPT = pathlib.Path(__file__).resolve().with_name("plasma_type.py")
# The first time, the portal waits for the person to answer its question.
DEADLINE = 90


def on_plasma() -> bool:
    desktops = os.environ.get("XDG_CURRENT_DESKTOP", "").upper().split(":")
    return "KDE" in desktops and bool(os.environ.get("WAYLAND_DISPLAY"))


def _system_gi() -> bool:
    """Whether the system Python has PyGObject, without starting it."""
    return any(glob.glob(pattern) for pattern in (
        "/usr/lib/python3*/site-packages/gi/__init__.py",
        "/usr/lib/python3/dist-packages/gi/__init__.py",
        "/usr/lib64/python3*/site-packages/gi/__init__.py",
    ))


def _plasma_ready() -> bool:
    """The availability probe: a Plasma Wayland session, and what plasma_type.py needs to run."""
    return on_plasma() and os.access(SYSTEM_PYTHON, os.X_OK) and SCRIPT.is_file() and _system_gi()


class PlasmaInjector(TextInjector):
    name = "plasma"

    def type_text(self, text: str) -> None:
        if not text:
            return
        try:
            done = subprocess.run(
                [SYSTEM_PYTHON, "-I", str(SCRIPT), "--auto"],
                input=text.encode("utf-8"), timeout=DEADLINE, check=False,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
        except (OSError, subprocess.SubprocessError):
            logger.error("could not run plasma_type.py", exc_info=True)
            return
        if done.returncode != 0:
            logger.error("typing failed: %s", done.stderr.decode("utf-8", "replace").strip()[:300])
            return
        control.publish({"event": "text", "text": text})

    def check_permissions(self, prompt: bool = True) -> tuple[bool, str]:
        if not on_plasma():
            return False, "Not a Plasma Wayland session."
        if not _system_gi():
            return False, (
                "The system Python has no PyGObject, which typing on Plasma needs for D-Bus.\n"
                "  Arch:   sudo pacman -S python-gobject\n"
                "  Debian: sudo apt install python3-gi\n"
                "  Fedora: sudo dnf install python3-gobject"
            )
        return True, (
            "The first time Mynah types, Plasma asks whether it may control the keyboard: "
            "allow it, and tick \"without asking next time\"."
        )
