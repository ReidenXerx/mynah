"""Tests for the shared tuning contract (tuning/tuning.toml).

The tuning file is the single source of truth for the dictate
segmentation pipeline's constants. Every implementation hardcodes the
values in its own language, and these tests pin each one against the
file so a drift fails loudly — the Swift suite mirrors this file in
macos/Tests/WhizAppTests/TuningTests.swift.

The file is deliberately NOT read at runtime by the engines. If a
value here changes, the constants must change in every implementation
at once (or the tests fail), which is the point.

Run with: pytest tests/test_tuning.py
"""

from __future__ import annotations

import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import pytest

from whiz import config as cfg
from whiz.dictate import engine as eng

TUNING_PATH = Path(__file__).resolve().parent.parent / "tuning" / "tuning.toml"


@pytest.fixture(scope="module")
def tuning() -> dict:
    with TUNING_PATH.open("rb") as fh:
        return tomllib.load(fh)


# ---------------------------------------------------------------------------
# Segmentation + calibration constants
# ---------------------------------------------------------------------------


def test_utterance_silence_matches_tuning(tuning: dict) -> None:
    assert eng._UTTERANCE_SILENCE == tuning["utterance_silence"]


def test_noise_calibration_constants_match_tuning(tuning: dict) -> None:
    assert eng._NOISE_CALIBRATION_SECONDS == tuning["noise_calibration_seconds"]
    assert eng._NOISE_FRAME_MULT == tuning["noise_frame_multiplier"]
    assert eng._NOISE_UTT_MULT == tuning["noise_utterance_multiplier"]
    assert eng._NOISE_MIN_SAMPLES == tuning["noise_min_samples"]
    assert eng._CALIBRATION_SPEECH_FLOOR == tuning["calibration_speech_floor"]


# ---------------------------------------------------------------------------
# Config defaults — whiz/config.py and DictateSettings
# ---------------------------------------------------------------------------


def test_config_defaults_match_tuning(tuning: dict) -> None:
    config = cfg.Config()
    assert config.dictate_frame_energy == tuning["frame_energy_default"]
    assert config.dictate_min_energy == tuning["min_energy_default"]
    assert config.dictate_min_utterance == tuning["min_utterance_default"]


def test_dictate_settings_defaults_match_tuning(tuning: dict) -> None:
    settings = eng.DictateSettings(
        language="ru",
        initial_prompt="",
        idle_timeout=45,
        auto_stop_silence=10,
        hotkey="<cmd>+<shift>+.",
        trigger="toggle",
        vad_enabled=True,
        show_indicator=True,
    )
    assert settings.frame_energy == tuning["frame_energy_default"]
    assert settings.min_energy == tuning["min_energy_default"]
    assert settings.min_utterance == tuning["min_utterance_default"]


# ---------------------------------------------------------------------------
# Hallucination phrase list — set equality, not subset
# ---------------------------------------------------------------------------


def test_hallucination_phrases_match_tuning(tuning: dict) -> None:
    assert eng._HALLUCINATION_PHRASES == frozenset(tuning["hallucination_phrases"])


# ---------------------------------------------------------------------------
# tuning.toml shape constraints — kept flat so the Swift FlatTOML
# parser can read the exact same file (it handles no nesting).
# ---------------------------------------------------------------------------


def test_tuning_toml_is_flat(tuning: dict) -> None:
    for key, value in tuning.items():
        assert isinstance(value, (float, int, str, list)), (
            f"key {key!r} has type {type(value).__name__} — FlatTOML on the "
            "Swift side handles flat scalars and string arrays only"
        )
        if isinstance(value, list):
            assert all(isinstance(v, str) for v in value), (
                f"key {key!r} is a non-string array — keep it a string array"
            )


def test_tuning_toml_keys_are_expected(tuning: dict) -> None:
    assert set(tuning.keys()) == {
        "utterance_silence",
        "trailing_padding",
        "noise_calibration_seconds",
        "noise_frame_multiplier",
        "noise_utterance_multiplier",
        "noise_min_samples",
        "calibration_speech_floor",
        "frame_energy_default",
        "min_energy_default",
        "min_utterance_default",
        "hallucination_phrases",
    }


# ---------------------------------------------------------------------------
# The golden corpus must be regenerable in place (byte-identical) — guards
# hand-edits to expected.json drifting from the WAVs.
# ---------------------------------------------------------------------------


def test_golden_corpus_is_regenerable() -> None:
    import subprocess

    golden_dir = TUNING_PATH.parent / "golden"
    expected_path = golden_dir / "expected.json"
    before = expected_path.read_bytes()

    subprocess.run(
        [sys.executable, str(golden_dir / "generate.py")],
        check=True,
        capture_output=True,
        cwd=golden_dir,
    )

    after = expected_path.read_bytes()
    assert after == before, (
        "tuning/golden/expected.json is not byte-identical to what "
        "generate.py produces — regenerate it: python3 tuning/golden/generate.py"
    )