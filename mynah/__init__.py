"""mynah — say it, and it types where you are.

A hotkey opens a session, your speech is segmented into utterances and each one
is transcribed and typed into whatever window has focus. Everything runs on the
machine you are sitting at.

Public entry point: ``from mynah import run_dictate``.
"""

from __future__ import annotations

__version__ = "0.14.0"

from mynah.engine import (
    DEFAULT_RUSSIAN_PROMPT,
    DictateSettings,
    DictationEngine,
    resolve_settings,
    run_dictate,
)

__all__ = [
    "DEFAULT_RUSSIAN_PROMPT",
    "DictateSettings",
    "DictationEngine",
    "resolve_settings",
    "run_dictate",
    "__version__",
]
