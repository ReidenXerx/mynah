"""Tests for the whiz dictate module.

Covers: provider selection/registry, VAD wrapper, engine settings resolution
(initial-prompt default, overrides), session lifecycle, idle-timeout unload,
utterance enqueueing + transcription routing, and indicator state/level —
all with heavy platform deps (mlx_whisper, sounddevice, pynput, Quartz/AppKit)
patched out so the tests run anywhere without the dictate extra installed.

Run with: pytest tests/test_dictate.py
"""

from __future__ import annotations

import sys
import types
from array import array
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from whiz import config as cfg
from whiz.dictate import engine as eng
from whiz.dictate.providers import base, list_providers
from whiz.dictate.providers.mlx import DEFAULT_MODEL, WHISPER_SAMPLE_RATE


# Inject fake sounddevice + numpy so the engine's _capture_loop can import them
# without the real (heavy / uninstalled) packages. This lets tests that call
# _start_session run the capture thread, and tests that call
# _transcribe_and_inject pass a working np, without numpy installed.


class _FakeDType:
    """Sentinel dtype that maps to an array.array typecode."""

    def __init__(self, code: str) -> None:
        self.code = code


_FAKE_INT16 = _FakeDType("h")
_FAKE_FLOAT32 = _FakeDType("f")


class _FakeArray:
    """Minimal ndarray-like wrapping array.array, enough for the engine/tests."""

    def __init__(self, arr: array) -> None:
        self._arr = arr

    def __len__(self) -> int:
        return len(self._arr)

    def tobytes(self) -> bytes:
        return self._arr.tobytes()

    def astype(self, dtype: _FakeDType) -> "_FakeArray":
        return _FakeArray(array(dtype.code, self._arr))

    def __truediv__(self, scalar: float) -> "_FakeArray":
        return _FakeArray(array("f", [v / scalar for v in self._arr]))


class _FakeNumpy(types.ModuleType):
    int16 = _FAKE_INT16
    float32 = _FAKE_FLOAT32

    @staticmethod
    def zeros(n: int, dtype: _FakeDType = _FAKE_FLOAT32) -> _FakeArray:
        return _FakeArray(array(dtype.code, [0] * n))

    @staticmethod
    def frombuffer(buf: bytes, dtype: _FakeDType = _FAKE_INT16) -> _FakeArray:
        return _FakeArray(array(dtype.code, buf))

    @staticmethod
    def sqrt(x: float) -> float:
        import math

        return math.sqrt(x)

    @staticmethod
    def mean(x) -> float:
        return sum(x) / len(x) if len(x) else 0.0


class _FakeInputStream:
    """Context-manager fake for sounddevice.InputStream — does nothing."""

    def __init__(self, **kwargs):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _FakeSoundDevice(types.ModuleType):
    InputStream = _FakeInputStream

    @staticmethod
    def sleep(ms):
        import time as _t

        _t.sleep(ms / 1000.0)


sys.modules.setdefault("numpy", _FakeNumpy("numpy"))
sys.modules.setdefault("sounddevice", _FakeSoundDevice("sounddevice"))


def _loud_pcm(seconds: float, amp: int = 20000) -> bytes:
    """Generate audible-energy int16 PCM (square wave) for tests.

    The engine's energy gate skips near-silent audio, so tests that expect
    transcription to proceed need realistic-energy audio, not zeros.
    ``amp`` defaults to 20000/32767 ≈ 0.61 — well above the _MIN_ENERGY floor.
    """
    n = int(WHISPER_SAMPLE_RATE * seconds)
    return array("h", (amp if i % 2 else -amp for i in range(n))).tobytes()


# ---------------------------------------------------------------------------
# Fakes — minimal stand-ins for the heavy deps.
# ---------------------------------------------------------------------------


class FakeSTT(base.STTProvider):
    """In-memory STT that returns a fixed string and tracks load/unload calls."""

    def __init__(self, text: str = "привет мир") -> None:
        self.text = text
        self._loaded = False
        self.load_calls = 0
        self.unload_calls = 0
        self.last_audio = None
        self.last_kwargs = {}

    @property
    def is_loaded(self) -> bool:
        return self._loaded

    def load(self) -> None:
        self.load_calls += 1
        self._loaded = True

    def transcribe(self, audio, sample_rate, language, initial_prompt) -> str:
        self.last_audio = audio
        self.last_kwargs = {
            "sample_rate": sample_rate,
            "language": language,
            "initial_prompt": initial_prompt,
        }
        return self.text

    def unload(self) -> None:
        self.unload_calls += 1
        self._loaded = False


class FakeInjector(base.TextInjector):
    def __init__(self) -> None:
        self.typed: list[str] = []
        self.permissions_ok = True

    def type_text(self, text: str) -> None:
        self.typed.append(text)

    def check_permissions(self) -> tuple[bool, str]:
        if self.permissions_ok:
            return True, ""
        return False, "grant Accessibility"


class FakeIndicator(base.DictationIndicator):
    def __init__(self) -> None:
        self.shown = False
        self.hidden = False
        self.levels: list[float] = []
        self.states: list[str] = []
        self.setup_called = False

    def setup(self) -> None:
        self.setup_called = True

    def show(self) -> None:
        self.shown = True

    def update_level(self, level: float) -> None:
        self.levels.append(level)

    def set_state(self, state: str) -> None:
        self.states.append(state)

    def hide(self) -> None:
        self.hidden = True


def _make_engine(
    *,
    stt: FakeSTT | None = None,
    injector: FakeInjector | None = None,
    indicator: FakeIndicator | None = None,
    settings: eng.DictateSettings | None = None,
) -> eng.DictationEngine:
    if settings is None:
        settings = eng.DictateSettings(
            language="ru",
            initial_prompt=eng.DEFAULT_RUSSIAN_PROMPT,
            idle_timeout=45,
            auto_stop_silence=10,
            hotkey="<cmd>+<shift>+.",
            trigger="toggle",
            vad_enabled=True,
            show_indicator=True,
            idle_visible=False,
            model="",
        )
    return eng.DictationEngine(
        settings,
        stt or FakeSTT(),
        injector or FakeInjector(),
        indicator or FakeIndicator(),
    )


# ---------------------------------------------------------------------------
# Provider registry
# ---------------------------------------------------------------------------


def test_list_providers_returns_stt_injector_indicator():
    provs = list_providers()
    assert "stt" in provs
    assert "injector" in provs
    assert "indicator" in provs
    # On macOS the built-in providers are registered.
    if sys.platform == "darwin":
        names = [n for n, _, _ in provs["stt"]]
        assert "mlx" in names


def test_select_indicator_returns_null_when_disabled():
    config = cfg.Config()
    config.dictate_show_indicator = False
    ind = __import__(
        "whiz.dictate.providers", fromlist=["select_indicator"]
    ).select_indicator(config)
    assert isinstance(ind, base.NullIndicator)


def test_select_stt_provider_override():
    """A valid config override selects that provider by name."""
    from whiz.dictate.providers import select_stt_provider

    config = cfg.Config()
    config.dictate_stt_provider = "mlx"
    # On macOS this should work; on other platforms the import thunk will
    # still resolve the name (it's registered regardless of platform).
    if sys.platform == "darwin":
        prov = select_stt_provider(config)
        assert prov is not None


# ---------------------------------------------------------------------------
# VAD wrapper
# ---------------------------------------------------------------------------


def test_vad_unavailable_fails_open():
    """When webrtcvad isn't installed, is_speech returns True (transcribe all)."""
    from whiz.dictate.vad import VoiceActivityDetector

    vad = VoiceActivityDetector()
    # webrtcvad is not installed in the test environment, so VAD should be
    # unavailable and is_speech should fail open (return True).
    if not vad.available:
        assert vad.is_speech(b"\x00" * 960) is True
    else:
        # If webrtcvad IS installed, just verify it doesn't crash on a valid frame.
        assert vad.is_speech(b"\x00" * 960) in (True, False)


def test_vad_frame_bytes_is_960_for_16khz_30ms():
    from whiz.dictate.vad import frame_bytes_for

    assert frame_bytes_for(16000, 30) == 960
    assert frame_bytes_for(8000, 30) == 480


# ---------------------------------------------------------------------------
# Settings resolution
# ---------------------------------------------------------------------------


def test_resolve_settings_uses_default_russian_prompt_when_empty():
    config = cfg.Config()
    config.dictate_prompt = ""
    s = eng.resolve_settings(config)
    assert s.initial_prompt == eng.DEFAULT_RUSSIAN_PROMPT


def test_resolve_settings_user_prompt_overrides_default():
    config = cfg.Config()
    config.dictate_prompt = "my custom prompt"
    s = eng.resolve_settings(config)
    assert s.initial_prompt == "my custom prompt"


def test_resolve_settings_cli_prompt_overrides_config():
    config = cfg.Config()
    config.dictate_prompt = "config prompt"
    s = eng.resolve_settings(config, prompt="cli prompt")
    assert s.initial_prompt == "cli prompt"


def test_resolve_settings_defaults():
    config = cfg.Config()
    s = eng.resolve_settings(config)
    assert s.language == "ru"
    assert s.idle_timeout == 45
    assert s.auto_stop_silence == 10
    assert s.hotkey == "<cmd>+<shift>+."
    assert s.vad_enabled is True
    assert s.show_indicator is True


def test_resolve_settings_overrides():
    config = cfg.Config()
    s = eng.resolve_settings(
        config,
        language="en",
        idle_timeout=30,
        auto_stop_silence=5,
        hotkey="<cmd>+d",
        show_indicator=False,
        model="custom-model",
    )
    assert s.language == "en"
    assert s.idle_timeout == 30
    assert s.auto_stop_silence == 5
    assert s.hotkey == "<cmd>+d"
    assert s.show_indicator is False
    assert s.model == "custom-model"


def test_default_russian_prompt_contains_obscenity():
    """The biasing prompt must contain Russian profanity so Whisper doesn't censor."""
    assert "пиздец" in eng.DEFAULT_RUSSIAN_PROMPT
    assert "ебать" in eng.DEFAULT_RUSSIAN_PROMPT
    assert "без цензуры" in eng.DEFAULT_RUSSIAN_PROMPT


# ---------------------------------------------------------------------------
# Idle-timeout lifecycle
# ---------------------------------------------------------------------------


def test_idle_unload_unloads_model_when_session_inactive():
    stt = FakeSTT()
    stt._loaded = True
    engine = _make_engine(stt=stt, settings=eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0.01,
        auto_stop_silence=0, hotkey="x", trigger="toggle", vad_enabled=False, show_indicator=False,
    ))
    engine._session_active = False
    engine._idle_unload()
    assert stt.unload_calls == 1
    assert stt.is_loaded is False


def test_idle_unload_skips_when_session_active():
    stt = FakeSTT()
    stt._loaded = True
    engine = _make_engine(stt=stt)
    engine._session_active = True
    engine._idle_unload()
    assert stt.unload_calls == 0
    assert stt.is_loaded is True


def test_schedule_idle_unload_zero_is_noop():
    stt = FakeSTT()
    engine = _make_engine(stt=stt, settings=eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0, hotkey="x", trigger="toggle", vad_enabled=False, show_indicator=False,
    ))
    engine._schedule_idle_unload()
    assert engine._idle_timer is None


# ---------------------------------------------------------------------------
# Session lifecycle (start/end)
# ---------------------------------------------------------------------------


def test_start_session_loads_model_and_shows_indicator():
    stt = FakeSTT()
    indicator = FakeIndicator()
    engine = _make_engine(stt=stt, indicator=indicator)
    engine._start_session()
    assert stt.load_calls == 1
    assert stt.is_loaded is True
    assert indicator.shown is True
    assert "listening" in indicator.states
    engine._end_session()


def test_start_session_skips_load_if_already_loaded():
    stt = FakeSTT()
    stt._loaded = True
    engine = _make_engine(stt=stt)
    engine._start_session()
    assert stt.load_calls == 0  # already loaded — no reload
    engine._end_session()


def test_end_session_flushes_remaining_utterance():
    stt = FakeSTT(text="остаток")
    injector = FakeInjector()
    engine = _make_engine(stt=stt, injector=injector)
    engine._start_session()
    # Simulate a buffered utterance that never got flushed by VAD.
    engine._utterance_buffer.append(_loud_pcm(2.0))
    engine._end_session()
    # The remaining utterance should have been transcribed + injected.
    assert "остаток" in injector.typed
    assert indicator_hidden(engine)


def indicator_hidden(engine) -> bool:
    return engine.indicator.hidden


def test_end_session_noop_when_not_active():
    stt = FakeSTT()
    engine = _make_engine(stt=stt)
    engine._end_session()  # should not raise
    assert stt.load_calls == 0


def test_toggle_session_starts_then_ends():
    stt = FakeSTT()
    indicator = FakeIndicator()
    engine = _make_engine(stt=stt, indicator=indicator)
    engine.toggle_session()
    assert engine._session_active is True
    assert indicator.shown is True
    engine.toggle_session()
    assert engine._session_active is False
    assert indicator.hidden is True


# ---------------------------------------------------------------------------
# Transcription routing
# ---------------------------------------------------------------------------


def test_transcribe_and_inject_types_text():
    import numpy as np

    stt = FakeSTT(text="привет")
    injector = FakeInjector()
    indicator = FakeIndicator()
    engine = _make_engine(stt=stt, injector=injector, indicator=indicator)
    pcm = _loud_pcm(1.0)
    engine._transcribe_and_inject(pcm, np)
    assert injector.typed == ["привет"]
    assert "transcribing" in indicator.states
    assert indicator.states[-1] == "listening"


def test_transcribe_and_inject_skips_too_short():
    import numpy as np

    stt = FakeSTT(text="x")
    injector = FakeInjector()
    engine = _make_engine(stt=stt, injector=injector)
    # 0.05s — below the 0.35s minimum.
    pcm = _loud_pcm(0.05)
    engine._transcribe_and_inject(pcm, np)
    assert injector.typed == []
    assert stt.last_audio is None


def test_transcribe_and_inject_passes_language_and_prompt():
    import numpy as np

    stt = FakeSTT(text="hi")
    engine = _make_engine(stt=stt)
    pcm = _loud_pcm(1.0)
    engine._transcribe_and_inject(pcm, np)
    assert stt.last_kwargs["language"] == "ru"
    assert stt.last_kwargs["initial_prompt"] == eng.DEFAULT_RUSSIAN_PROMPT


def test_transcribe_and_inject_skips_empty_text():
    import numpy as np

    stt = FakeSTT(text="   ")  # whitespace only
    injector = FakeInjector()
    engine = _make_engine(stt=stt, injector=injector)
    pcm = _loud_pcm(1.0)
    engine._transcribe_and_inject(pcm, np)
    assert injector.typed == []


def test_transcribe_and_inject_swallows_stt_error():
    import numpy as np

    stt = FakeSTT()
    stt.transcribe = mock.Mock(side_effect=RuntimeError("boom"))
    indicator = FakeIndicator()
    engine = _make_engine(stt=stt, indicator=indicator)
    pcm = _loud_pcm(1.0)
    engine._transcribe_and_inject(pcm, np)  # should not raise
    # Indicator should return to listening after the error.
    assert indicator.states[-1] == "listening"


def test_transcribe_and_inject_skips_silence_energy_gate():
    """All-silent audio is skipped by the energy gate (no hallucination)."""
    import numpy as np

    stt = FakeSTT(text="спасибо за субтитры")  # would-be hallucination
    injector = FakeInjector()
    engine = _make_engine(stt=stt, injector=injector)
    pcm = (np.zeros(WHISPER_SAMPLE_RATE, dtype=np.int16)).tobytes()
    engine._transcribe_and_inject(pcm, np)
    assert injector.typed == []
    assert stt.last_audio is None


def test_transcribe_and_inject_suppresses_hallucination_phrase():
    """Known hallucination phrases are suppressed even if audio passes energy gate."""
    import numpy as np

    stt = FakeSTT(text="Спасибо за субтитры Алексею Дубровскому!")
    injector = FakeInjector()
    engine = _make_engine(stt=stt, injector=injector)
    pcm = _loud_pcm(1.0)
    engine._transcribe_and_inject(pcm, np)
    assert injector.typed == []


def test_transcribe_and_inject_suppresses_to_be_continued():
    """'Продолжение следует…' hallucination is suppressed."""
    import numpy as np

    stt = FakeSTT(text="Продолжение следует...")
    injector = FakeInjector()
    engine = _make_engine(stt=stt, injector=injector)
    pcm = _loud_pcm(1.0)
    engine._transcribe_and_inject(pcm, np)
    assert injector.typed == []


# ---------------------------------------------------------------------------
# Indicator
# ---------------------------------------------------------------------------


def test_indicator_states_on_session():
    indicator = FakeIndicator()
    engine = _make_engine(indicator=indicator)
    engine._start_session()
    # Cold load → transcribing, then listening.
    assert "transcribing" in indicator.states
    assert "listening" in indicator.states
    engine._end_session()
    assert "idle" in indicator.states


def test_indicator_update_level_records_values():
    indicator = FakeIndicator()
    engine = _make_engine(indicator=indicator)
    engine.indicator.update_level(0.3)
    engine.indicator.update_level(0.8)
    assert indicator.levels == [0.3, 0.8]


def test_run_calls_indicator_setup_before_loop(monkeypatch):
    """run() must call indicator.setup() once on the main thread before the
    run loop starts, so macOS can create its NSPanel off the hotkey thread.
    """
    indicator = FakeIndicator()
    engine = _make_engine(indicator=indicator)
    # Force the plain loop path so we don't need AppKit; setup() still runs.
    monkeypatch.setattr(eng, "_is_macos", lambda: False)
    # Make _run_plain exit immediately by pre-setting the stop event.
    engine._stop_event.set()
    engine.run()
    assert indicator.setup_called is True


# ---------------------------------------------------------------------------
# NullIndicator
# ---------------------------------------------------------------------------


def test_null_indicator_is_inert():
    ni = base.NullIndicator()
    ni.show()
    ni.update_level(0.5)
    ni.set_state("listening")
    ni.hide()
    # No state to assert — just ensure no exception.


# ---------------------------------------------------------------------------
# MLX provider constants
# ---------------------------------------------------------------------------


def test_mlx_default_model_is_turbo_full_precision():
    # The default is the full-precision mlx-whisper turbo repo. q4 quant-
    # ization degrades recognition (garbled output), so we use the unquant-
    # ized model. Do NOT confuse with the -4bit mlx-audio-plus repos.
    assert "large-v3-turbo" in DEFAULT_MODEL
    assert "mlx-community" in DEFAULT_MODEL
    # Must NOT be a quantized variant (q4, q8, 4bit, 8bit).
    for suffix in ("-q4", "-q8", "-4bit", "-8bit"):
        assert not DEFAULT_MODEL.endswith(suffix), f"{DEFAULT_MODEL} is quantized"


def test_whisper_sample_rate_is_16k():
    assert WHISPER_SAMPLE_RATE == 16000


# ---------------------------------------------------------------------------
# Trigger mode (toggle vs push-to-talk)
# ---------------------------------------------------------------------------


def test_resolve_settings_default_trigger_is_toggle():
    config = cfg.Config()
    s = eng.resolve_settings(config)
    assert s.trigger == "toggle"


def test_resolve_settings_trigger_from_config():
    config = cfg.Config()
    config.dictate_trigger = "ptt"
    s = eng.resolve_settings(config)
    assert s.trigger == "ptt"


def test_resolve_settings_trigger_cli_overrides_config():
    config = cfg.Config()
    config.dictate_trigger = "toggle"
    s = eng.resolve_settings(config, trigger="ptt")
    assert s.trigger == "ptt"


def test_resolve_settings_trigger_normalized_lowercase():
    config = cfg.Config()
    config.dictate_trigger = "PTT"
    s = eng.resolve_settings(config)
    assert s.trigger == "ptt"


def test_ptt_press_starts_session():
    stt = FakeSTT()
    indicator = FakeIndicator()
    engine = _make_engine(stt=stt, indicator=indicator)
    engine.ptt_press()
    assert engine._session_active is True
    assert stt.load_calls == 1
    assert indicator.shown is True
    engine._end_session()


def test_ptt_release_ends_session():
    stt = FakeSTT()
    indicator = FakeIndicator()
    engine = _make_engine(stt=stt, indicator=indicator)
    engine.ptt_press()
    assert engine._session_active is True
    engine.ptt_release()
    assert engine._session_active is False
    assert indicator.hidden is True


def test_ptt_press_noop_if_already_active():
    stt = FakeSTT()
    engine = _make_engine(stt=stt)
    engine.ptt_press()
    assert stt.load_calls == 1
    # Second press while active should not reload.
    engine.ptt_press()
    assert stt.load_calls == 1
    engine._end_session()


def test_ptt_release_noop_if_not_active():
    stt = FakeSTT()
    engine = _make_engine(stt=stt)
    engine.ptt_release()  # should not raise
    assert stt.load_calls == 0
    assert engine._session_active is False


def test_toggle_mode_uses_toggle_session():
    """In toggle mode, the same callback flips the session on then off."""
    stt = FakeSTT()
    engine = _make_engine(stt=stt)
    assert engine.s.trigger == "toggle"
    engine.toggle_session()
    assert engine._session_active is True
    engine.toggle_session()
    assert engine._session_active is False


# ---------------------------------------------------------------------------
# CLI: friendly key mapping + dictate config/set subcommands
# ---------------------------------------------------------------------------


def test_dictate_friendly_keys_map_to_config_fields():
    from whiz.cli import _DICTATE_FRIENDLY_KEYS

    assert _DICTATE_FRIENDLY_KEYS["hotkey"] == "dictate_hotkey"
    assert _DICTATE_FRIENDLY_KEYS["trigger"] == "dictate_trigger"
    assert _DICTATE_FRIENDLY_KEYS["language"] == "dictate_language"
    assert _DICTATE_FRIENDLY_KEYS["lang"] == "dictate_language"  # alias
    assert _DICTATE_FRIENDLY_KEYS["idle"] == "dictate_idle_timeout"  # alias
    assert _DICTATE_FRIENDLY_KEYS["silence"] == "dictate_auto_stop_silence"  # alias
    assert _DICTATE_FRIENDLY_KEYS["indicator"] == "dictate_show_indicator"  # alias


def test_dictate_friendly_keys_cover_all_dictate_config_fields():
    """Every dictate_* config field should be reachable via a friendly name."""
    from whiz.cli import _DICTATE_FRIENDLY_KEYS

    mapped_fields = set(_DICTATE_FRIENDLY_KEYS.values())
    for field_name in cfg.Config.__dataclass_fields__:
        if field_name.startswith("dictate_"):
            assert field_name in mapped_fields, f"{field_name} has no friendly key"


def test_dictate_config_fields_table_has_all_fields():
    from whiz.cli import _DICTATE_CONFIG_FIELDS

    field_keys = {key for key, _, _ in _DICTATE_CONFIG_FIELDS}
    for field_name in cfg.Config.__dataclass_fields__:
        if field_name.startswith("dictate_"):
            assert field_name in field_keys, f"{field_name} missing from config table"


def test_cmd_dictate_set_hotkey(tmp_path, monkeypatch):
    """`whiz dictate set hotkey=<f8>` persists dictate_hotkey."""
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="hotkey=<f8>")
    rc = cli.cmd_dictate_set(args)
    assert rc == 0
    saved = cfg.load()
    assert saved.dictate_hotkey == "<f8>"


def test_cmd_dictate_set_trigger_ptt(tmp_path, monkeypatch):
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="trigger=ptt")
    rc = cli.cmd_dictate_set(args)
    assert rc == 0
    saved = cfg.load()
    assert saved.dictate_trigger == "ptt"


def test_cmd_dictate_set_trigger_invalid_rejected(tmp_path, monkeypatch):
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="trigger=bogus")
    with pytest.raises(SystemExit):
        cli.cmd_dictate_set(args)


def test_cmd_dictate_set_unknown_key_rejected(tmp_path, monkeypatch):
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="bogus_key=value")
    with pytest.raises(SystemExit):
        cli.cmd_dictate_set(args)


def test_cmd_dictate_set_missing_equals_rejected(tmp_path, monkeypatch):
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="hotkey")
    with pytest.raises(SystemExit):
        cli.cmd_dictate_set(args)


def test_cmd_dictate_set_lang_alias(tmp_path, monkeypatch):
    """The 'lang' alias should map to dictate_language."""
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="lang=en")
    rc = cli.cmd_dictate_set(args)
    assert rc == 0
    saved = cfg.load()
    assert saved.dictate_language == "en"


def test_cmd_dictate_set_bool_indicator(tmp_path, monkeypatch):
    """Setting indicator (bool) to false persists dictate_show_indicator=false."""
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="indicator=false")
    rc = cli.cmd_dictate_set(args)
    assert rc == 0
    saved = cfg.load()
    assert saved.dictate_show_indicator is False


def test_cmd_dictate_config_shows_settings(tmp_path, monkeypatch):
    """`whiz dictate config` should print all dictate settings without error."""
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock()
    rc = cli.cmd_dictate_config(args)
    assert rc == 0


def test_dictate_parser_bare_runs_dictation():
    """`whiz dictate` (no subcommand) should set func=cmd_dictate."""
    from whiz import cli

    parser = cli.build_parser()
    args = parser.parse_args(["dictate"])
    assert args.func is cli.cmd_dictate
    assert args.dictate_command is None


def test_dictate_parser_config_subcommand():
    from whiz import cli

    parser = cli.build_parser()
    args = parser.parse_args(["dictate", "config"])
    assert args.func is cli.cmd_dictate_config


def test_dictate_parser_set_subcommand():
    from whiz import cli

    parser = cli.build_parser()
    args = parser.parse_args(["dictate", "set", "hotkey=<f8>"])
    assert args.func is cli.cmd_dictate_set
    assert args.assignment == "hotkey=<f8>"


def test_dictate_parser_trigger_flag():
    from whiz import cli

    parser = cli.build_parser()
    args = parser.parse_args(["dictate", "--trigger", "ptt"])
    assert args.trigger == "ptt"
    assert args.func is cli.cmd_dictate


# ---------------------------------------------------------------------------
# Wave-2 microscope fixes: concurrency, auto-stop, capture-thread join,
# PTT modifiers, config-set validation, stop idempotency.
# ---------------------------------------------------------------------------


def test_end_session_joins_capture_thread():
    """_end_session must join the capture thread so it can't be reused.

    Regression for the two-stream race on rapid PTT re-trigger: the old
    capture thread must be joined (and its stream closed) before a new
    session can start another.
    """
    engine = _make_engine()
    engine._start_session()
    cap = engine._capture_thread
    assert cap is not None
    engine._end_session()
    assert not cap.is_alive()
    assert engine._capture_thread is None


def test_end_session_flushes_buffer_after_capture_stops():
    """The utterance buffer is flushed only after the capture thread is gone,
    closing the two-writer race (audio callback vs _end_session)."""
    stt = FakeSTT(text="хвост")
    injector = FakeInjector()
    engine = _make_engine(stt=stt, injector=injector)
    engine._start_session()
    engine._utterance_buffer.append(_loud_pcm(2.0))
    engine._end_session()
    assert "хвост" in injector.typed
    assert engine._utterance_buffer == []


def test_auto_stop_sets_flag_not_blocking_call():
    """Auto-stop must set _end_session_requested, not call _end_session
    synchronously (which would block the audio callback on a thread join)."""
    engine = _make_engine(settings=eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0.5, hotkey="x", trigger="toggle",
        vad_enabled=True, show_indicator=False,
    ))
    # webrtcvad isn't installed in the test env, so patch the VAD with a fake
    # that reports itself available and classifies every frame as silence —
    # that's the path that triggers auto-stop.
    class _FakeVad:
        available = True
        frame_bytes = 960
        def is_speech(self, frame):
            return False
    engine._vad = _FakeVad()
    engine._start_session()
    frame_bytes = 960
    frame_seconds = frame_bytes / 2 / WHISPER_SAMPLE_RATE
    frames_needed = int(0.5 / frame_seconds) + 2
    engine._in_speech = False
    for _ in range(frames_needed):
        engine._process_vad_frames(b"\x00" * frame_bytes, frame_bytes, frame_seconds)
    assert engine._end_session_requested is True
    assert engine._session_active is True  # capture loop ends it, not the callback
    assert engine._capturing is False
    engine._end_session()


def test_start_session_idempotent_under_rapid_press():
    """A second _start_session while already active must not reload or spawn
    a second capture/transcribe thread pair."""
    stt = FakeSTT()
    engine = _make_engine(stt=stt)
    engine._start_session()
    first_cap = engine._capture_thread
    first_tr = engine._transcribe_thread
    loads = stt.load_calls
    engine._start_session()  # rapid double-press
    assert stt.load_calls == loads
    assert engine._capture_thread is first_cap
    assert engine._transcribe_thread is first_tr
    engine._end_session()


def test_stop_is_idempotent_and_ends_active_session():
    """stop() is self-locking and idempotent."""
    stt = FakeSTT()
    injector = FakeInjector()
    engine = _make_engine(stt=stt, injector=injector)
    engine._start_session()
    engine.stop()
    assert engine._session_active is False
    engine.stop()  # second stop must be a no-op, not raise
    assert engine._session_active is False


def test_start_session_aborts_if_stop_races_during_load():
    """If a stop/end arrives while the model is loading (outside the lock),
    _start_session must not start capture threads — it aborts cleanly."""
    stt = FakeSTT()
    engine = _make_engine(stt=stt)

    def racing_load():
        stt._loaded = True
        stt.load_calls += 1
        engine._session_active = False  # concurrent stop during load
    stt.load = racing_load
    engine._start_session()
    assert engine._capture_thread is None
    assert engine._transcribe_thread is None


def _install_fake_pynput(monkeypatch, hotkey_parse):
    import types as _types

    class FakeKey:
        def __init__(self, name):
            self.name = name
        def __eq__(self, other):
            return isinstance(other, FakeKey) and self.name == other.name
        def __hash__(self):
            return hash(self.name)
        def __repr__(self):
            return f"Key.{self.name}"

    class FakeListener:
        def __init__(self, on_press=None, on_release=None):
            self.on_press = on_press
            self.on_release = on_release
        def start(self):
            pass
        def stop(self):
            pass

    class FakeGlobalHotKeys:
        def __init__(self, activations):
            self.activations = activations
        def start(self):
            pass
        def stop(self):
            pass

    class FakeHotKey:
        parse = staticmethod(hotkey_parse)

    kb = _types.ModuleType("pynput.keyboard")
    kb.HotKey = FakeHotKey
    kb.Listener = FakeListener
    kb.GlobalHotKeys = FakeGlobalHotKeys
    kb.Key = FakeKey
    monkeypatch.setitem(sys.modules, "pynput", _types.ModuleType("pynput"))
    monkeypatch.setitem(sys.modules, "pynput.keyboard", kb)
    return kb


def test_ptt_listener_requires_modifiers_for_combo(monkeypatch):
    """A combo hotkey in PTT mode must NOT fire on the bare final key."""
    captured = {}

    def parse(s):
        out = []
        for tok in s.split("+"):
            tok = tok.strip()
            if tok == "<ctrl>":
                out.append(captured["K"]("ctrl_l"))
            elif tok == "<space>":
                out.append(captured["K"]("space"))
        return out

    kb = _install_fake_pynput(monkeypatch, parse)
    captured["K"] = kb.Key
    engine = _make_engine(settings=eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0, hotkey="<ctrl>+<space>", trigger="ptt",
        vad_enabled=False, show_indicator=False,
    ))
    listener = engine._start_hotkey_listener()
    assert listener is not None
    K = kb.Key
    # Bare Space (no ctrl held) must NOT start dictation.
    listener.on_press(K("space"))
    assert engine._session_active is False
    # Hold ctrl, then press space -> starts.
    listener.on_press(K("ctrl_l"))
    listener.on_press(K("space"))
    assert engine._session_active is True
    # Release space -> stops.
    listener.on_release(K("space"))
    assert engine._session_active is False


def test_ptt_listener_single_key_no_modifiers(monkeypatch):
    """A single-key PTT hotkey (<f8>) fires on press with no modifiers held."""
    captured = {}

    def parse(s):
        return [captured["K"]("f8")] if s == "<f8>" else []

    kb = _install_fake_pynput(monkeypatch, parse)
    captured["K"] = kb.Key
    engine = _make_engine(settings=eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0, hotkey="<f8>", trigger="ptt",
        vad_enabled=False, show_indicator=False,
    ))
    listener = engine._start_hotkey_listener()
    K = kb.Key
    listener.on_press(K("f8"))
    assert engine._session_active is True
    listener.on_release(K("f8"))
    assert engine._session_active is False


def test_toggle_listener_uses_global_hot_keys(monkeypatch):
    """Toggle mode must use GlobalHotKeys (not the nonexistent GlobalHotKeyListener)
    and register the hotkey combo with an activate callback."""
    kb = _install_fake_pynput(monkeypatch, lambda s: [])
    engine = _make_engine(settings=eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0, hotkey="<cmd>+<shift>+.", trigger="toggle",
        vad_enabled=False, show_indicator=False,
    ))
    listener = engine._start_hotkey_listener()
    assert listener is not None
    assert hasattr(listener, "activations")
    assert "<cmd>+<shift>+." in listener.activations


def test_toggle_listener_invalid_hotkey_stops_engine(monkeypatch):
    """An unparseable hotkey must set _stop_event (fatal), not return None and
    leave the engine running with no way to trigger it — a listenerless agent
    under KeepAlive would otherwise silently loop forever."""
    def _raising_parse(_s):
        raise ValueError("<period>")

    _install_fake_pynput(monkeypatch, _raising_parse)
    engine = _make_engine(settings=eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0, hotkey="<cmd>+<shift>+<period>", trigger="toggle",
        vad_enabled=False, show_indicator=False,
    ))
    listener = engine._start_hotkey_listener()
    assert listener is None
    assert engine._stop_event.is_set()


def test_ptt_listener_invalid_hotkey_stops_engine(monkeypatch):
    """Same fatal-stop guarantee for PTT mode."""
    def _raising_parse(_s):
        raise ValueError("<period>")

    _install_fake_pynput(monkeypatch, _raising_parse)
    engine = _make_engine(settings=eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0, hotkey="<cmd>+<shift>+<period>", trigger="ptt",
        vad_enabled=False, show_indicator=False,
    ))
    listener = engine._start_hotkey_listener()
    assert listener is None
    assert engine._stop_event.is_set()


def test_auto_stop_ends_session_from_capture_loop(monkeypatch):
    """Auto-stop fires _end_session from the capture thread itself. The join
    of the capture thread must be skipped (a thread cannot join itself),
    and the session must end cleanly without a RuntimeError."""
    engine = _make_engine(settings=eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0.5, hotkey="x", trigger="toggle",
        vad_enabled=True, show_indicator=False,
    ))
    class _FakeVad:
        available = True
        frame_bytes = 960
        def is_speech(self, frame):
            return False
    engine._vad = _FakeVad()
    engine._start_session()
    # Simulate the capture loop: run _capture_loop's auto-stop tail on the
    # capture thread itself, by setting the flag then calling _end_session.
    import threading as _t
    cap = engine._capture_thread
    err = {}
    def from_capture_thread():
        try:
            engine._end_session_requested = False
            engine._end_session()
        except Exception as e:  # noqa: BLE001
            err["e"] = e
    th = _t.Thread(target=from_capture_thread)
    th.start()
    th.join()
    assert "e" not in err, f"_end_session from capture thread raised: {err.get('e')}"
    assert engine._session_active is False
    # The capture thread reference is cleared by _end_session.
    assert engine._capture_thread is None


def test_cmd_config_set_validates_dictate_trigger(tmp_path, monkeypatch):
    """`whiz config set dictate_trigger=bogus` must be rejected."""
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="dictate_trigger=bogus")
    with pytest.raises(SystemExit):
        cli.cmd_config_set(args)


def test_cmd_config_set_accepts_valid_dictate_trigger(tmp_path, monkeypatch):
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="dictate_trigger=ptt")
    rc = cli.cmd_config_set(args)
    assert rc == 0
    assert cfg.load().dictate_trigger == "ptt"


def test_default_russian_prompt_has_no_duplicate_words():
    """The bias prompt should not repeat tokens (was: 'ебать' twice)."""
    seen = set()
    for w in eng.DEFAULT_RUSSIAN_PROMPT.replace(",", " ").split():
        w = w.strip(".,;:!?\"")
        if not w:
            continue
        assert w not in seen, f"duplicate word in prompt: {w}"
        seen.add(w)


def test_mic_error_path_ends_session(monkeypatch):
    """A microphone error must end the session (hide the indicator), not just
    set the stop event and leave the overlay visible."""
    indicator = FakeIndicator()
    engine = _make_engine(indicator=indicator)
    engine._start_session()
    import sounddevice as sd

    class _ErrStream:
        def __init__(self, **kwargs):
            raise RuntimeError("No input device available")
        def __enter__(self):
            return self
        def __exit__(self, *exc):
            return False

    monkeypatch.setattr(sd, "InputStream", _ErrStream)
    engine._capturing = True
    engine._capture_loop()
    assert engine._stop_event.is_set()
    assert engine._session_active is False
    assert indicator.hidden is True


# ---------------------------------------------------------------------------
# Idle-visible indicator + login LaunchAgent service
# ---------------------------------------------------------------------------


def test_resolve_settings_idle_visible_default_true():
    config = cfg.Config()
    s = eng.resolve_settings(config)
    assert s.idle_visible is True


def test_resolve_settings_idle_visible_override():
    config = cfg.Config()
    config.dictate_idle_visible = False
    s = eng.resolve_settings(config)
    assert s.idle_visible is False
    s = eng.resolve_settings(config, idle_visible=True)
    assert s.idle_visible is True


def test_run_shows_idle_indicator_when_idle_visible(monkeypatch):
    """run() must show the dimmed idle badge as soon as the service starts
    when idle_visible is on, so the user can see dictation is armed."""
    indicator = FakeIndicator()
    settings = eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0, hotkey="x", trigger="toggle",
        vad_enabled=False, show_indicator=True, idle_visible=True,
    )
    engine = _make_engine(indicator=indicator, settings=settings)
    monkeypatch.setattr(eng, "_is_macos", lambda: False)
    engine._stop_event.set()
    engine.run()
    assert indicator.setup_called is True
    assert indicator.shown is True
    assert "idle" in indicator.states


def test_run_hides_indicator_when_idle_visible_false(monkeypatch):
    """When idle_visible is off, run() must NOT show the indicator at idle —
    only setup() runs (the original hide-until-session behavior)."""
    indicator = FakeIndicator()
    settings = eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0, hotkey="x", trigger="toggle",
        vad_enabled=False, show_indicator=True, idle_visible=False,
    )
    engine = _make_engine(indicator=indicator, settings=settings)
    monkeypatch.setattr(eng, "_is_macos", lambda: False)
    engine._stop_event.set()
    engine.run()
    assert indicator.setup_called is True
    assert indicator.shown is False


def test_end_session_shows_idle_badge_when_idle_visible():
    """After a session ends with idle_visible, the indicator returns to the
    dimmed idle badge (show), not hidden."""
    indicator = FakeIndicator()
    settings = eng.DictateSettings(
        language="ru", initial_prompt="x", idle_timeout=0,
        auto_stop_silence=0, hotkey="x", trigger="toggle",
        vad_enabled=False, show_indicator=True, idle_visible=True,
    )
    engine = _make_engine(indicator=indicator, settings=settings)
    engine._start_session()
    indicator.shown = False  # reset to observe the end-of-session show
    engine._end_session()
    assert indicator.states[-1] == "idle"
    assert indicator.shown is True
    assert indicator.hidden is False


def test_end_session_hides_when_idle_visible_false():
    """Without idle_visible, _end_session hides the indicator (legacy behavior)."""
    indicator = FakeIndicator()
    engine = _make_engine(indicator=indicator)
    assert engine.s.idle_visible is False
    engine._start_session()
    engine._end_session()
    assert indicator.hidden is True


def test_dictate_set_idle_visible(tmp_path, monkeypatch):
    """`whiz dictate set idle_visible=false` persists dictate_idle_visible."""
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="idle_visible=false")
    rc = cli.cmd_dictate_set(args)
    assert rc == 0
    assert cfg.load().dictate_idle_visible is False


def test_dictate_set_idle_badge_alias(tmp_path, monkeypatch):
    """The 'idle_badge' alias maps to dictate_idle_visible."""
    from whiz import cli

    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    args = mock.Mock(assignment="idle_badge=true")
    rc = cli.cmd_dictate_set(args)
    assert rc == 0
    assert cfg.load().dictate_idle_visible is True


# --- LaunchAgent service module ---


def test_service_build_plist_contains_required_keys(monkeypatch):
    from whiz.dictate import service

    # Force a deterministic whiz binary resolution.
    monkeypatch.setattr(service.shutil, "which", lambda _name: "/usr/local/bin/whiz")
    xml = service.build_plist()
    assert service.LABEL in xml
    assert "<key>RunAtLoad</key>" in xml
    assert "<true/>" in xml
    assert "<key>KeepAlive</key>" in xml
    assert "<key>ThrottleInterval</key>" in xml
    assert "<key>ProcessType</key>" in xml
    assert "Interactive" in xml
    assert "/usr/local/bin/whiz" in xml
    assert "dictate" in xml  # ProgramArguments includes the subcommand
    assert "whiz-dictate.log" in xml


def test_service_build_plist_falls_back_to_python_m(monkeypatch):
    from whiz.dictate import service

    monkeypatch.setattr(service.shutil, "which", lambda _name: None)
    xml = service.build_plist()
    assert "-m" in xml
    assert "whiz" in xml
    assert "dictate" in xml


def test_service_install_writes_plist_and_loads(monkeypatch, tmp_path):
    from whiz.dictate import service

    plist = tmp_path / f"{service.LABEL}.plist"
    log = tmp_path / "whiz-dictate.log"
    monkeypatch.setattr(service, "_LAUNCH_AGENTS_DIR", tmp_path)
    monkeypatch.setattr(service, "_LOG_PATH", log)
    monkeypatch.setattr(service.shutil, "which", lambda _name: "/usr/local/bin/whiz")

    calls = []

    def fake_run(cmd, **kwargs):
        calls.append(cmd)
        return mock.Mock(returncode=0, stdout="", stderr="")

    monkeypatch.setattr(service.subprocess, "run", fake_run)
    monkeypatch.setattr(service, "_run", fake_run)

    rc = service.install()
    assert rc == 0
    assert plist.exists()
    assert plist.read_text().startswith("<?xml")
    # Must have attempted to load the plist.
    assert any(c[:2] == ["launchctl", "load"] for c in calls), calls


def test_service_unload_on_reinstall(monkeypatch, tmp_path):
    """install() unloads an existing plist before loading the new one."""
    from whiz.dictate import service

    plist = tmp_path / f"{service.LABEL}.plist"
    log = tmp_path / "whiz-dictate.log"
    plist.write_text("<old/>")
    monkeypatch.setattr(service, "_LAUNCH_AGENTS_DIR", tmp_path)
    monkeypatch.setattr(service, "_LOG_PATH", log)
    monkeypatch.setattr(service.shutil, "which", lambda _name: "/usr/local/bin/whiz")

    calls = []

    def fake_run(cmd, **kwargs):
        calls.append(cmd)
        return mock.Mock(returncode=0, stdout="", stderr="")

    monkeypatch.setattr(service.subprocess, "run", fake_run)
    monkeypatch.setattr(service, "_run", fake_run)

    service.install()
    assert any(c[:2] == ["launchctl", "unload"] for c in calls), calls


def test_service_uninstall_removes_plist(monkeypatch, tmp_path):
    from whiz.dictate import service

    plist = tmp_path / f"{service.LABEL}.plist"
    plist.write_text("<old/>")
    monkeypatch.setattr(service, "_LAUNCH_AGENTS_DIR", tmp_path)

    calls = []

    def fake_run(cmd, **kwargs):
        calls.append(cmd)
        return mock.Mock(returncode=0, stdout="", stderr="")

    monkeypatch.setattr(service.subprocess, "run", fake_run)
    monkeypatch.setattr(service, "_run", fake_run)

    rc = service.uninstall()
    assert rc == 0
    assert not plist.exists()
    assert any(c[:2] == ["launchctl", "unload"] for c in calls), calls


def test_service_uninstall_when_not_installed(monkeypatch, tmp_path):
    from whiz.dictate import service

    monkeypatch.setattr(service, "_LAUNCH_AGENTS_DIR", tmp_path)

    def fake_run(cmd, **kwargs):
        return mock.Mock(returncode=0, stdout="", stderr="")

    monkeypatch.setattr(service.subprocess, "run", fake_run)
    monkeypatch.setattr(service, "_run", fake_run)
    rc = service.uninstall()
    assert rc == 0


def test_service_status_not_loaded(monkeypatch, tmp_path):
    from whiz.dictate import service

    monkeypatch.setattr(service, "_LAUNCH_AGENTS_DIR", tmp_path)

    def fake_run(cmd, **kwargs):
        return mock.Mock(returncode=1, stdout="", stderr="not loaded")

    monkeypatch.setattr(service.subprocess, "run", fake_run)
    monkeypatch.setattr(service, "_run", fake_run)
    rc = service.status()
    assert rc == 0  # not-loaded is not an error


def test_service_status_loaded_parses_pid(monkeypatch, tmp_path):
    from whiz.dictate import service

    monkeypatch.setattr(service, "_LAUNCH_AGENTS_DIR", tmp_path)
    sample = '{\n    "PID" = 4242;\n    "LastExitStatus" = 0;\n}'

    def fake_run(cmd, **kwargs):
        return mock.Mock(returncode=0, stdout=sample, stderr="")

    monkeypatch.setattr(service.subprocess, "run", fake_run)
    monkeypatch.setattr(service, "_run", fake_run)
    rc = service.status()
    assert rc == 0


def test_dictate_parser_service_subcommands():
    from whiz import cli

    parser = cli.build_parser()
    for action in ("install", "uninstall", "status"):
        args = parser.parse_args(["dictate", "service", action])
        assert args.func is cli.cmd_dictate_service
        assert args.service_action == action


def test_dictate_parser_service_remove_alias():
    from whiz import cli

    parser = cli.build_parser()
    args = parser.parse_args(["dictate", "service", "remove"])
    assert args.service_action == "uninstall"


def test_cmd_dictate_service_install_refuses_without_extra(monkeypatch, capsys):
    """install must refuse (rc=1) when the dictate extra isn't installed,
    rather than writing a LaunchAgent that would crash-loop on startup."""
    import builtins
    from whiz import cli

    real_import = builtins.__import__

    def fake_import(name, *args, **kwargs):
        if name in {"sounddevice", "pynput"}:
            raise ImportError(name)
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", fake_import)
    args = mock.Mock(service_action="install")
    rc = cli.cmd_dictate_service(args)
    assert rc == 1
    out = capsys.readouterr()
    assert "dictate' extra is not installed" in out.err
    assert "crash-loop" in out.err


# ---------------------------------------------------------------------------
# Guided first-time setup / doctor (whiz dictate setup)
# ---------------------------------------------------------------------------


def test_setup_check_extra_passes_when_importable(monkeypatch):
    """_check_extra reports ok when the dictate extra deps import cleanly."""
    from whiz.dictate import setup as setup_mod

    # The test env injects fake sounddevice; force the rest to import ok.
    monkeypatch.setitem(sys.modules, "pynput", types.ModuleType("pynput"))
    monkeypatch.setitem(sys.modules, "webrtcvad", types.ModuleType("webrtcvad"))
    monkeypatch.setitem(sys.modules, "AppKit", types.ModuleType("AppKit"))
    monkeypatch.setitem(sys.modules, "Quartz", types.ModuleType("Quartz"))
    monkeypatch.setitem(sys.modules, "ApplicationServices", types.ModuleType("ApplicationServices"))
    r = setup_mod._check_extra()
    assert r.ok is True
    assert r.title == "Dictate extra"


def test_setup_check_extra_fails_when_missing(monkeypatch):
    """_check_extra reports not-ok and an inject hint when deps are missing."""
    from whiz.dictate import setup as setup_mod

    import builtins

    real_import = builtins.__import__

    def fake_import(name, *args, **kwargs):
        if name == "pynput":
            raise ImportError(name)
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", fake_import)
    r = setup_mod._check_extra()
    assert r.ok is False
    assert "pynput" in r.detail
    assert "pipx inject" in r.hint


def test_setup_check_accessibility_passes_when_trusted(monkeypatch):
    from whiz.dictate import setup as setup_mod

    fake_appsvc = types.ModuleType("ApplicationServices")
    fake_appsvc.AXIsProcessTrustedWithOptions = lambda opts: True
    fake_cf = types.ModuleType("CoreFoundation")
    fake_cf.kCFBooleanTrue = True
    fake_foundation = types.ModuleType("Foundation")
    fake_foundation.NSDictionary = mock.Mock()
    monkeypatch.setitem(sys.modules, "ApplicationServices", fake_appsvc)
    monkeypatch.setitem(sys.modules, "CoreFoundation", fake_cf)
    monkeypatch.setitem(sys.modules, "Foundation", fake_foundation)
    r = setup_mod._check_accessibility()
    assert r.ok is True
    assert r.title == "Accessibility"


def test_setup_check_accessibility_fails_when_not_trusted(monkeypatch):
    from whiz.dictate import setup as setup_mod

    fake_appsvc = types.ModuleType("ApplicationServices")
    fake_appsvc.AXIsProcessTrustedWithOptions = lambda opts: False
    fake_cf = types.ModuleType("CoreFoundation")
    fake_cf.kCFBooleanTrue = True
    fake_foundation = types.ModuleType("Foundation")
    fake_foundation.NSDictionary = mock.Mock()
    monkeypatch.setitem(sys.modules, "ApplicationServices", fake_appsvc)
    monkeypatch.setitem(sys.modules, "CoreFoundation", fake_cf)
    monkeypatch.setitem(sys.modules, "Foundation", fake_foundation)
    r = setup_mod._check_accessibility()
    assert r.ok is False
    assert "Accessibility" in r.hint


def test_setup_check_microphone_passes(monkeypatch):
    """_check_microphone reports ok when an InputStream opens cleanly."""
    from whiz.dictate import setup as setup_mod

    class _OkStream:
        def __init__(self, **kw):
            pass
        def __enter__(self):
            return self
        def __exit__(self, *exc):
            return False

    import sounddevice as sd
    monkeypatch.setattr(sd, "InputStream", _OkStream)
    r = setup_mod._check_microphone()
    assert r.ok is True
    assert r.title == "Microphone"


def test_setup_check_microphone_fails_on_denial(monkeypatch):
    """_check_microphone reports not-ok with a grant hint when the stream
    raises an input/device error (the macOS permission-denied shape)."""
    from whiz.dictate import setup as setup_mod

    class _DenyStream:
        def __init__(self, **kw):
            raise RuntimeError("No input device available")

    import sounddevice as sd
    monkeypatch.setattr(sd, "InputStream", _DenyStream)
    r = setup_mod._check_microphone()
    assert r.ok is False
    assert "Microphone" in r.hint


def test_setup_check_hotkey_passes_on_valid_default(monkeypatch, tmp_path):
    """_check_hotkey reports ok when the configured hotkey parses with pynput."""
    from whiz.dictate import setup as setup_mod

    # Install a fake pynput.keyboard with a parse that accepts the default.
    fake_kb = types.ModuleType("pynput.keyboard")
    fake_kb.HotKey = mock.Mock()
    fake_kb.HotKey.parse = staticmethod(lambda s: ["parsed"])
    monkeypatch.setitem(sys.modules, "pynput", types.ModuleType("pynput"))
    monkeypatch.setitem(sys.modules, "pynput.keyboard", fake_kb)
    # Isolate config so the default hotkey is used.
    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    r = setup_mod._check_hotkey()
    assert r.ok is True
    assert r.title == "Hotkey"
    assert "Valid" in r.detail


def test_setup_check_hotkey_fails_on_invalid(monkeypatch, tmp_path):
    """_check_hotkey reports not-ok with a fix hint when parse raises."""
    from whiz.dictate import setup as setup_mod

    fake_kb = types.ModuleType("pynput.keyboard")
    fake_kb.HotKey = mock.Mock()
    def _raising(_s):
        raise ValueError("<period>")
    fake_kb.HotKey.parse = staticmethod(_raising)
    monkeypatch.setitem(sys.modules, "pynput", types.ModuleType("pynput"))
    monkeypatch.setitem(sys.modules, "pynput.keyboard", fake_kb)
    monkeypatch.setattr(cfg, "CONFIG_DIR", tmp_path)
    monkeypatch.setattr(cfg, "CONFIG_PATH", tmp_path / "config.toml")
    r = setup_mod._check_hotkey()
    assert r.ok is False
    assert "Invalid" in r.detail
    assert "whiz dictate set hotkey" in r.hint


def test_setup_run_checks_returns_three_results():
    from whiz.dictate import setup as setup_mod

    results = setup_mod.run_checks()
    assert len(results) == 4
    titles = [r.title for r in results]
    assert titles == ["Dictate extra", "Accessibility", "Microphone", "Hotkey"]


def test_setup_all_pass_points_at_service(monkeypatch, capsys):
    """When all checks pass and the service isn't loaded, setup points the
    user at `whiz dictate service install` (doesn't auto-install)."""
    from whiz.dictate import setup as setup_mod

    monkeypatch.setattr(setup_mod, "run_checks", lambda: [
        setup_mod.CheckResult(ok=True, title="Dictate extra", detail="ok"),
        setup_mod.CheckResult(ok=True, title="Accessibility", detail="ok"),
        setup_mod.CheckResult(ok=True, title="Microphone", detail="ok"),
        setup_mod.CheckResult(ok=True, title="Hotkey", detail="ok"),
    ])
    monkeypatch.setattr(setup_mod, "_service_loaded", lambda: False)
    rc = setup_mod.setup()
    assert rc == 0
    out = capsys.readouterr()
    assert "All checks passed" in out.err
    assert "whiz dictate service install" in out.err


def test_setup_all_pass_notes_running_service(monkeypatch, capsys):
    """When all checks pass and the service is already loaded, note that."""
    from whiz.dictate import setup as setup_mod

    monkeypatch.setattr(setup_mod, "run_checks", lambda: [
        setup_mod.CheckResult(ok=True, title="Dictate extra", detail="ok"),
        setup_mod.CheckResult(ok=True, title="Accessibility", detail="ok"),
        setup_mod.CheckResult(ok=True, title="Microphone", detail="ok"),
        setup_mod.CheckResult(ok=True, title="Hotkey", detail="ok"),
    ])
    monkeypatch.setattr(setup_mod, "_service_loaded", lambda: True)
    rc = setup_mod.setup()
    assert rc == 0
    out = capsys.readouterr()
    assert "already installed and running" in out.err


def test_setup_failure_returns_one_and_recheck_hint(monkeypatch, capsys):
    """A failing check returns rc=1 and tells the user to re-run setup."""
    from whiz.dictate import setup as setup_mod

    monkeypatch.setattr(setup_mod, "run_checks", lambda: [
        setup_mod.CheckResult(ok=True, title="Dictate extra", detail="ok"),
        setup_mod.CheckResult(ok=False, title="Accessibility", detail="no", hint="grant it"),
        setup_mod.CheckResult(ok=True, title="Microphone", detail="ok"),
        setup_mod.CheckResult(ok=True, title="Hotkey", detail="ok"),
    ])
    rc = setup_mod.setup()
    assert rc == 1
    out = capsys.readouterr()
    assert "Some checks failed" in out.err
    assert "whiz dictate setup" in out.err


def test_dictate_parser_setup_subcommand():
    from whiz import cli

    parser = cli.build_parser()
    args = parser.parse_args(["dictate", "setup"])
    assert args.func is cli.cmd_dictate_setup


def test_dictate_parser_doctor_alias():
    from whiz import cli

    parser = cli.build_parser()
    args = parser.parse_args(["dictate", "doctor"])
    assert args.func is cli.cmd_dictate_setup
