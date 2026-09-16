"""The indicator on Linux: publish the state, let the desktop draw it.

On macOS the indicator *is* a window — an NSPanel mynah owns and animates. On
Linux drawing our own always-on-top pill would mean picking a toolkit, fighting
layer-shell, and looking foreign on every desktop it lands on. The shell
already draws things; it just needs to be told.

So this indicator renders nothing. It publishes ``state`` and ``level`` events
on the control socket, and the Omarchy plugin (or anything else reading
``mynah watch``) draws the pill in the desktop's own idiom.

When nothing is subscribed, publishing is a no-op — dictation works exactly the
same with no shell plugin installed, it just has no face.
"""

from __future__ import annotations

from mynah import control
from mynah.providers.base import DictationIndicator


class SocketIndicator(DictationIndicator):
    """Broadcasts what the macOS indicator would have drawn."""

    name = "socket"

    def __init__(self) -> None:
        self._state = "idle"
        self._visible = False

    def setup(self) -> None:
        # Nothing to create: there is no window on this platform.
        pass

    def show(self) -> None:
        self._visible = True
        control.publish({"event": "visible", "visible": True})

    def update_level(self, level: float) -> None:
        # Rate-limited in control.publish_level: this is called for every 30 ms
        # frame, and no UI can use 33 updates a second.
        control.publish_level(level)

    def set_state(self, state: str) -> None:
        if state == self._state:
            return
        self._state = state
        control.publish({"event": "state", "state": state})

    def hide(self) -> None:
        self._visible = False
        control.publish({"event": "visible", "visible": False})
