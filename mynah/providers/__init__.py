"""Provider registry — platform detection + config overrides.

The engine calls ``select_stt_provider(config)`` etc. to get the right
concrete provider for the current platform. Config overrides
(``stt_provider`` / ``injector`` / ``indicator``)
let a user force a specific provider by name; empty = auto-detect by
platform.

Each provider has a short ``name`` (e.g. ``"mlx"``, ``"mac"``) used for
the config override and ``--list-providers``.

Registering a new platform's providers: add the import + class to the
appropriate ``_register_*`` function below.

A platform may register more than one provider for the same job (Linux ships
whisper.cpp today and may ship others later), so auto-detection asks each
candidate whether it can actually run here before settling on the first one.
"""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING

from mynah.providers.base import (
    DictationIndicator,
    NullIndicator,
    STTProvider,
    TextInjector,
)

if TYPE_CHECKING:  # pragma: no cover - typing only
    from mynah.config import Config

# Provider name -> (constructor, platform). Constructors are thunks so we
# never import a platform's heavy deps (mlx_whisper, pyobjc, ...) unless
# that provider is actually selected. This keeps ``mynah
# --list-providers`` and tests fast and import-safe on non-macOS.

_STT_PROVIDERS: dict[str, tuple[str, callable]] = {}
_INJECTORS: dict[str, tuple[str, callable]] = {}
_INDICATORS: dict[str, tuple[str, callable]] = {}

# Optional "can this actually run on this machine?" probes, by provider name.
# A provider without one is assumed usable on its platform. Probes must be
# cheap and side-effect free: they run on every auto-detect.
_AVAILABLE: dict[str, callable] = {}


def _register_linux() -> None:
    """Linux providers: whisper.cpp for speech, wtype for typing, the socket
    for the indicator (the desktop shell draws it — see linux_indicator.py)."""
    _STT_PROVIDERS["whisper-cpp"] = ("linux", lambda: _import_attr(
        "mynah.providers.linux_stt", "WhisperCppProvider"))
    _AVAILABLE["whisper-cpp"] = lambda: _probe("mynah.providers.linux_stt", "find_binary")
    # Registered first, so auto-detect picks it: types with wtype, and pastes
    # into the apps that ignore a virtual keyboard's keymap (see
    # linux_clipboard.SmartInjector).
    _INJECTORS["smart"] = ("linux", lambda: _import_attr(
        "mynah.providers.linux_clipboard", "SmartInjector"))
    _AVAILABLE["smart"] = lambda: _probe("mynah.providers.linux_inject", "find_binary")
    _INJECTORS["wtype"] = ("linux", lambda: _import_attr(
        "mynah.providers.linux_inject", "WtypeInjector"))
    _AVAILABLE["wtype"] = lambda: _probe("mynah.providers.linux_inject", "find_binary")
    _INJECTORS["clipboard"] = ("linux", lambda: _import_attr(
        "mynah.providers.linux_clipboard", "ClipboardInjector"))
    _AVAILABLE["clipboard"] = lambda: _probe("mynah.providers.linux_clipboard", "_clipboard_ready")
    _INDICATORS["socket"] = ("linux", lambda: _import_attr(
        "mynah.providers.linux_indicator", "SocketIndicator"))


def _probe(module: str, func: str) -> bool:
    """True if ``module.func()`` finds what the provider needs."""
    try:
        mod = __import__(module, fromlist=[func])
        return bool(getattr(mod, func)())
    except Exception:  # noqa: BLE001
        return False


def _register_macos() -> None:
    if sys.platform != "darwin":
        # Still register the names so --list-providers can mention them as
        # "available on macOS", but defer the import to selection time so a
        # non-macOS machine doesn't crash on the pyobjc import.
        pass
    _STT_PROVIDERS["mlx"] = ("darwin", lambda: _import_attr(
        "mynah.providers.mlx", "MlxWhisperProvider"))
    _INJECTORS["mac"] = ("darwin", lambda: _import_attr(
        "mynah.providers.macos_inject", "MacTextInjector"))
    _INDICATORS["mac"] = ("darwin", lambda: _import_attr(
        "mynah.providers.macos_indicator", "MacIndicator"))


def _import_attr(module: str, attr: str):
    mod = __import__(module, fromlist=[attr])
    return getattr(mod, attr)()


def _platform_default(platform: str | None, table: dict[str, tuple[str, callable]]):
    """Pick a provider registered for ``platform`` (or the current platform).

    Prefers one whose availability probe passes, so a machine with two possible
    backends gets the installed one rather than the first-registered one. With
    none available, fall back to the first for the platform: its constructor
    raises a message naming what to install, which beats "no provider".
    """
    plat = platform or sys.platform
    if plat.startswith("linux"):
        plat = "linux"
    candidates = [name for name, (supports, _ctor) in table.items() if supports == plat]
    for name in candidates:
        probe = _AVAILABLE.get(name)
        if probe is None or probe():
            return name
    return candidates[0] if candidates else None


def select_stt_provider(config: Config) -> STTProvider:
    """Return the STT provider for this platform (or the configured override)."""
    override = (config.stt_provider or "").strip()
    if override and override in _STT_PROVIDERS:
        _supports, ctor = _STT_PROVIDERS[override]
        return ctor()
    name = _platform_default(None, _STT_PROVIDERS)
    if name is None:
        raise RuntimeError(
            f"No STT provider available for platform '{sys.platform}'. "
            "Set one with: mynah set stt_provider=..."
        )
    return _STT_PROVIDERS[name][1]()


def select_injector(config: Config) -> TextInjector:
    """Return the text injector for this platform (or the configured override)."""
    override = (config.injector or "").strip()
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
    if not config.show_indicator:
        return NullIndicator()
    override = (config.indicator or "").strip()
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
    Used by ``mynah providers``.
    """
    plat = platform or sys.platform
    if plat.startswith("linux"):
        plat = "linux"
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
_register_linux()