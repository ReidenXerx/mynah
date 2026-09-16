"""Mynah's settings.

They live at ``~/.config/mynah/config.toml``, written by both this CLI and the
macOS app. Reading uses stdlib ``tomllib``; writing is a small hand-rolled TOML
emitter, so a dictation daemon carries no config dependency.

Mynah grew out of ``whiz dictate``, so the first run imports the ``dictate_*``
keys from ``~/.config/whiz/config.toml`` if they are there. Nobody should have to
pick their hotkey and model a second time because a project was split in two.
"""

from __future__ import annotations

import os
import tomllib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

CONFIG_DIR = Path(os.environ.get("MYNAH_CONFIG_DIR", Path.home() / ".config" / "mynah"))
CONFIG_PATH = CONFIG_DIR / "config.toml"
# Where the settings lived while dictation was part of whiz.
LEGACY_PATH = Path(os.environ.get("MYNAH_LEGACY_CONFIG", Path.home() / ".config" / "whiz" / "config.toml"))


@dataclass
class Config:
    # --- what listens ---
    # Global hotkey in pynput syntax. The '.' key is a literal character, NOT a
    # named key — pynput's HotKey.parse rejects '<period>' with ValueError, so
    # it has to be written as a bare '.'.
    hotkey: str = "<cmd>+<shift>+."
    # "toggle" (press to start, press again to stop) or "ptt" (hold to talk).
    trigger: str = "toggle"
    # WebRTC VAD for utterance segmentation. Off means the whole session is
    # transcribed as one block when it ends, so text only appears at the end.
    vad: bool = True
    # Seconds of continuous silence before a session stops itself (0 = off).
    auto_stop_silence: float = 10.0

    # --- what it hears ---
    # Spoken language code.
    language: str = "ru"
    # Speech model: a repo id or a path. Empty means the provider's default.
    model: str = ""
    # initial_prompt, to bias recognition. Empty means the built-in one.
    prompt: str = ""
    # Seconds to keep the model loaded after a session before unloading
    # (0 = never unload; the default keeps back-to-back dictation instant).
    idle_timeout: float = 45.0

    # --- how loud is speech ---
    # Static energy floors for segmentation (normalized RMS, 0.0-1.0). These
    # are FLOORS: the adaptive noise calibration at session start can raise the
    # effective gates above them, never lower them, and its contribution is
    # itself capped at the speech floor (see tuning/tuning.toml). Raise these
    # if noise is being transcribed; lower them if you have to raise your voice.
    frame_energy: float = 0.010
    min_energy: float = 0.008
    # Shortest utterance worth transcribing, in seconds. Below this it is a
    # click or a breath — though short words ("да", "yes") can fall under 0.35.
    min_utterance: float = 0.25

    # --- what you see ---
    show_indicator: bool = True
    # Keep the indicator dimmed-visible while idle, not only during a session.
    idle_visible: bool = False
    menu_bar: bool = True

    # --- which implementation ---
    # Empty means auto-detect for the platform.
    stt_provider: str = ""
    injector: str = ""
    indicator: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def _read_toml(path: Path) -> dict[str, Any]:
    with path.open("rb") as fh:
        return tomllib.load(fh)


def legacy_values() -> dict[str, Any]:
    """The ``dictate_*`` keys from whiz's config, with the prefix stripped."""
    if not LEGACY_PATH.exists():
        return {}
    try:
        data = _read_toml(LEGACY_PATH)
    except (OSError, tomllib.TOMLDecodeError):
        return {}
    out: dict[str, Any] = {}
    for key, value in data.items():
        if not key.startswith("dictate_"):
            continue
        name = key[len("dictate_"):]
        if name in Config.__dataclass_fields__:
            out[name] = value
    return out


def load() -> Config:
    """Settings from disk, or the defaults.

    A MISSING file means defaults, which is fine. A CORRUPT one must not be
    silently swallowed into defaults: the next ``mynah set`` would then
    read-modify-write from an empty table and delete every key the user had.
    Say so instead, and name the file.
    """
    if CONFIG_PATH.exists():
        try:
            data = _read_toml(CONFIG_PATH)
        except tomllib.TOMLDecodeError as e:
            raise RuntimeError(
                f"config.toml is corrupt and could not be read: {e}\n"
                f"Fix or delete the file by hand: {CONFIG_PATH}\n"
                "(a deleted file regenerates from defaults; a corrupt one left "
                "in place breaks every command until it is fixed)"
            ) from e
        known = {k: v for k, v in data.items() if k in Config.__dataclass_fields__}
        return Config(**known)

    imported = legacy_values()
    if imported:
        config = Config(**imported)
        save(config)
        return config
    return Config()


def save(cfg: Config) -> Path:
    """Write the settings, keeping keys this version has never heard of.

    Read-modify-write rather than a plain overwrite, because the file has more
    than one writer: this CLI, the macOS app, and whatever version of either
    happens to be installed. ``load()`` filters to known fields, so a naive
    overwrite would silently delete the others' settings.
    """
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    merged: dict[str, Any] = {}
    if CONFIG_PATH.exists():
        try:
            merged.update(_read_toml(CONFIG_PATH))
        except (OSError, tomllib.TOMLDecodeError):
            merged = {}
    merged.update({k: v for k, v in cfg.to_dict().items() if v is not None})
    CONFIG_PATH.write_text(_emit_toml(merged), encoding="utf-8")
    return CONFIG_PATH


def _escape(value: str) -> str:
    """Escape a string for a basic TOML double-quoted literal.

    Backslash first, or its own output gets re-escaped.
    """
    out = value.replace("\\", "\\\\").replace('"', '\\"')
    return out.replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def _format(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, list):
        return "[" + ", ".join(_format(item) for item in value) + "]"
    return '"' + _escape(str(value)) + '"'


def _emit_toml(data: dict[str, Any]) -> str:
    lines = [f"{key} = {_format(value)}" for key, value in sorted(data.items()) if value is not None]
    return "\n".join(lines) + "\n"
