"""Provider interfaces for the dictation engine.

Three pluggable provider kinds keep the core engine platform-agnostic:

- STTProvider:          load/transcribe/unload a speech-to-text model.
- TextInjector:         type transcribed text into the focused application.
- DictationIndicator:  show a floating "listening" overlay with a live
                        volume curve.

Each platform (macOS now, Linux/Windows later) supplies concrete
implementations. The engine depends only on these ABCs, so adding a new
platform never touches ``engine.py`` — just add a provider module and
register it in ``providers/__init__.py``.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import TYPE_CHECKING

if TYPE_CHECKING:  # pragma: no cover - typing only
    import numpy as np


class STTProvider(ABC):
    """A speech-to-text backend (e.g. mlx-whisper on macOS, faster-whisper elsewhere)."""

    @abstractmethod
    def load(self) -> None:
        """Load the model into memory (cold start). May take seconds."""

    @property
    @abstractmethod
    def is_loaded(self) -> bool:
        """True if the model is loaded and ready to transcribe."""

    @abstractmethod
    def transcribe(
        self,
        audio: np.ndarray,
        sample_rate: int,
        language: str,
        initial_prompt: str,
    ) -> str:
        """Transcribe a mono float32 audio array, returning the recognized text."""

    @abstractmethod
    def unload(self) -> None:
        """Release the model from memory (free RAM at idle)."""


class TextInjector(ABC):
    """Injects text into the currently focused application."""

    @abstractmethod
    def type_text(self, text: str) -> None:
        """Type ``text`` into whatever app currently has keyboard focus."""

    @abstractmethod
    def check_permissions(self) -> tuple[bool, str]:
        """Verify platform permissions are granted.

        Returns ``(ok, hint)`` where ``hint`` is a human-readable remediation
        message shown when ``ok`` is False (e.g. "grant Accessibility in
        System Settings → Privacy & Security").
        """


class DictationIndicator(ABC):
    """A floating overlay that shows the user dictation is active.

    The engine feeds it live mic amplitude (0.0–1.0) so it can animate a
    volume curve, and notifies it of state transitions (listening →
    transcribing → idle).
    """

    def setup(self) -> None:
        """Perform any main-thread setup before the run loop starts.

        Called once on the main thread from the engine's ``run()`` before
        the hotkey listener starts. Platforms whose indicator needs
        main-thread-only APIs (e.g. macOS ``NSWindow`` instantiation) create
        their UI here; the default is a no-op.
        """

    @abstractmethod
    def show(self) -> None:
        """Display the overlay."""

    @abstractmethod
    def update_level(self, level: float) -> None:
        """Feed a live mic amplitude in [0.0, 1.0] to animate the volume curve."""

    @abstractmethod
    def set_state(self, state: str) -> None:
        """Notify the indicator of a state change.

        ``state`` is one of: ``"listening"``, ``"transcribing"``, ``"idle"``.
        """

    @abstractmethod
    def hide(self) -> None:
        """Dismiss the overlay."""


# Sentinel returned by a no-op indicator when the overlay is disabled
# (``--no-indicator`` or ``dictate_show_indicator = false``). Avoids a None
# check on every engine → indicator call.
class NullIndicator(DictationIndicator):
    """A no-op indicator for headless/quiet use — all methods are inert."""

    def setup(self) -> None:
        pass

    def show(self) -> None:
        pass

    def update_level(self, level: float) -> None:
        pass

    def set_state(self, state: str) -> None:
        pass

    def hide(self) -> None:
        pass