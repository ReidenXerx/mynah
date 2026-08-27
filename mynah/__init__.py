"""whiz dictate — system-wide voice dictation.

Public entry point: ``from whiz.dictate import run_dictate``.
"""

from __future__ import annotations

from whiz.dictate.engine import (
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
]