"""Provider registry — platform detection + config overrides.

The engine calls ``select_stt_provider(config)`` etc. to get the right
concrete provider for the current platform. Config overrides
(``dictate_stt_provider`` / ``dictate_injector`` / ``dictate_indicator``)
let a user force a specific provider by name; empty = auto-detect by
platform.

Each provider has a short ``name`` (e.g. ``"mlx"``, ``"mac"``) used for
the config override and ``--list-providers``.

Registering a new platform's providers: add the import + class to the
appropriate ``_PLATFORM_*`` map below.
"""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING

from whiz.dictate.providers.base import (
    DictationIndicator,
    NullIndicator,
    STTProvider,
    TextInjector,
)

if TYPE_CHECKING:  # pragma: no cover - typing only
    from whiz.config import Config

# Provider name -> (constructor, platform). Constructors are thunks so we
# never import a platform's heavy deps (mlx_whisper, pyobjc, ...) unless
# that provider is actually selected. This keeps ``whiz dictate
# --list-providers`` and tests fast and import-safe on non-macOS.

_STT_PROVIDERS: dict[str, tuple[str, callable]] = {}
_INJECTORS: dict[str, tuple[str, callable]] = {}
_INDICATORS: dict[str, tuple[str, callable]] = {}


def _register_macos() -> None:
    if sys.platform != "darwin":
        # Still register the names so --list-providers can mention them as
        # "available on macOS", but defer the import to selection time so a
        # non-macOS machine doesn't crash on the pyobjc import.
        pass
    _STT_PROVIDERS["mlx"] = ("darwin", lambda: _import_attr(
        "whiz.dictate.providers.mlx", "MlxWhisperProvider"))
    _INJECTORS["mac"] = ("darwin", lambda: _import_attr(
        "whiz.dictate.providers.macos_inject", "MacTextInjector"))
    # The floating pill NSPanel indicator is dropped (per user decision) —
    # the rumps menu bar W icon now serves as the sole visual indicator on
    # macOS. The macOS indicator returns NullIndicator so the engine doesn't
    # try to create an NSPanel from a LaunchAgent (which couldn't render).
    # macos_indicator.py is retained for posterity but no longer selected.
    _INDICATORS["mac"] = ("darwin", lambda: NullIndicator())


def _import_attr(module: str, attr: str):
    mod = __import__(module, fromlist=[attr])
    return getattr(mod, attr)()


def _platform_default(platform: str | None, table: dict[str, tuple[str, callable]]):
    """Pick the first provider registered for ``platform`` (or current platform)."""
    plat = platform or sys.platform
    for _name, (supports, _ctor) in table.items():
        if supports == plat:
            return _name
    return None


def select_stt_provider(config: Config) -> STTProvider:
    """Return the STT provider for this platform (or the configured override)."""
    override = (config.dictate_stt_provider or "").strip()
    if override and override in _STT_PROVIDERS:
        _supports, ctor = _STT_PROVIDERS[override]
        return ctor()
    name = _platform_default(None, _STT_PROVIDERS)
    if name is None:
        raise RuntimeError(
            f"No STT provider available for platform '{sys.platform}'. "
            "Set one with: whiz config set dictate_stt_provider=..."
        )
    return _STT_PROVIDERS[name][1]()


def select_injector(config: Config) -> TextInjector:
    """Return the text injector for this platform (or the configured override)."""
    override = (config.dictate_injector or "").strip()
    if override and override in _INJECTORS:
        return _INJECTORS[override][1]()
    name = _platform_default(None, _INJECTORS)
    if name is None:
        raise RuntimeError(
            f"No text injector available for platform '{sys.platform}'."
        )
    return _INJECTORS[name][1]()


def select_indicator(config: Config) -> DictationIndicator:
    """Return the dictation indicator, or a NullIndicator when disabled."""
    if not config.dictate_show_indicator:
        return NullIndicator()
    override = (config.dictate_indicator or "").strip()
    if override and override in _INDICATORS:
        return _INDICATORS[override][1]()
    name = _platform_default(None, _INDICATORS)
    if name is None:
        # No indicator for this platform — degrade silently to no overlay
        # rather than failing the whole dictation.
        return NullIndicator()
    return _INDICATORS[name][1]()


def list_providers(platform: str | None = None) -> dict[str, list[tuple[str, str, bool]]]:
    """List available providers for ``platform`` (default: current).

    Returns ``{"stt": [(name, supports_platform, current_platform)], ...}``.
    Used by ``whiz dictate --list-providers``.
    """
    plat = platform or sys.platform
    out: dict[str, list[tuple[str, str, bool]]] = {"stt": [], "injector": [], "indicator": []}
    for name, (supports, _ctor) in _STT_PROVIDERS.items():
        out["stt"].append((name, supports, supports == plat))
    for name, (supports, _ctor) in _INJECTORS.items():
        out["injector"].append((name, supports, supports == plat))
    for name, (supports, _ctor) in _INDICATORS.items():
        out["indicator"].append((name, supports, supports == plat))
    return out


# Register built-in providers on import.
_register_macos()