"""Golden tests: the real engine must segment the shared corpus as pinned.

Drives the REAL DictationEngine — not the reference implementation in
tuning/golden/generate.py — through its actual audio callback against
the fixtures in tuning/golden/, whose expectations are pinned in
expected.json. The reference in generate.py exists to WRITE the corpus;
this test proves the production engine READS the same audio the same
way. The Swift suite runs the same fixtures through
UtteranceDetector (macos/Tests/WhizAppTests/TuningTests.swift), so a
segmentation change that breaks cross-implementation agreement fails
on both platforms.

How it drives the engine:
- The engine is constructed with ``vad_enabled=True`` so the real
  energy-gate state machine (``_process_vad_frames``) runs — that is
  the shared contract. The engine's VoiceActivityDetector is then
  replaced with an always-speech stub: production VAD (webrtcvad /
  Silero) is implementation-specific secondary confirmation, excluded
  from the golden contract, and unavailable in the test env anyway.
- sounddevice is faked (as tests/test_dictate.py does) so the capture
  loop's InputStream replays the WAV frame by frame into the engine's
  real callback.

What is asserted, per region (see expected.json):
- ``start``: the time of the frame at which the region opens.
- ``end``: the start time of the frame that closed it. The engine's
  buffered PCM spans [start, end + frame) — Python keeps ALL trailing
  silence, so end+frame is the true buffer extent.
- ``rejected_by_energy_gate``: the whole-buffer RMS gate in
  ``_transcribe_and_inject``, evaluated against the padded buffer
  (what Python feeds the gate) and the CALIBRATED
  ``_effective_min_energy`` (raised in noisy rooms; speech-aware —
  speech in the window is excluded, never measured).

Not asserted (known divergences, documented in docs/ARCHITECTURE.md):
- The min-utterance length gate: Python applies it to the padded
  buffer, Swift to the trimmed one.
- Swift's trimming of trailing silence to trailing_padding.

Run with: pytest tests/test_segmentation_golden.py
"""

from __future__ import annotations

import array
import json
import math
import sys
import types
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import pytest

from whiz.dictate import engine as eng
from whiz.dictate.providers import base

GOLDEN_DIR = Path(__file__).resolve().parent.parent / "tuning" / "golden"
EXPECTED = GOLDEN_DIR / "expected.json"

SAMPLE_RATE = 16000
FRAME_BYTES = 960          # 30 ms of int16 at 16 kHz
FRAME_SAMPLES = FRAME_BYTES // 2  # 480
FRAME_SECONDS = FRAME_SAMPLES / SAMPLE_RATE

CASES = [
    "quiet_two_utterances",
    "speech_during_calibration",
    "speech_late_in_calibration",
    "noisy_room",
    "click_below_min",
    "trailing_silence_trim",
    "gap_below_silence",
    "speech_over_noise_in_calibration",
]


class _RecordingSTT(base.STTProvider):
    """STT that records buffers it is asked to transcribe (post-gate)."""

    def __init__(self) -> None:
        self.received: list[bytes] = []
        self._loaded = False

    @property
    def is_loaded(self) -> bool:
        return self._loaded

    def load(self) -> None:
        self._loaded = True

    def transcribe(self, audio, sample_rate, language, initial_prompt) -> str:
        self.received.append(audio)
        return ""

    def unload(self) -> None:
        self._loaded = False


class _NullInjector(base.TextInjector):
    def type_text(self, text: str) -> None:
        pass

    def check_permissions(self, prompt: bool = True) -> tuple[bool, str]:
        return True, ""


class _NullIndicator(base.DictationIndicator):
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


class _AlwaysSpeechVAD:
    """Stands in for VoiceActivityDetector: available, and every frame
    that clears the energy gate is speech.

    The energy pre-filter in _process_vad_frames runs BEFORE is_speech,
    so with this stub the segmentation decision reduces to the energy
    gate alone — exactly the golden contract (VAD excluded).
    ``frame_bytes`` must be 960 so the engine chunks the PCM stream
    into 30 ms frames.
    """

    available = True
    frame_bytes = FRAME_BYTES

    def is_speech(self, frame: bytes) -> bool:
        return True


class _MonoColumn:
    """The minimal ndarray-like the engine's callback needs.

    engine.py does:
        mono = indata[:, 0]
        pcm = (mono * 32767).astype(np.int16).tobytes()
        rms = float(np.sqrt(np.mean(mono ** 2)))
    ``astype`` returns self: the callback then calls ``tobytes`` on it,
    which quantizes the float samples back to int16 — the same
    round-trip production audio takes (float32 in, int16 for VAD).
    """

    def __init__(self, chunk: list[float]) -> None:
        self._chunk = chunk

    def __getitem__(self, key):
        # indata[:, 0]
        if key == (slice(None), 0):
            return self
        raise KeyError(key)

    def __len__(self) -> int:
        return len(self._chunk)

    def __mul__(self, scalar: float):
        return _MonoColumn([v * scalar for v in self._chunk])

    def __pow__(self, exp: int):
        if exp == 2:
            return _MonoColumn([v * v for v in self._chunk])
        raise NotImplementedError(exp)

    def astype(self, dtype) -> "_MonoColumn":
        # np.int16 — the callback only ever converts to int16.
        return self

    def tobytes(self) -> bytes:
        raw = array.array("h", (round(v) for v in self._chunk))
        return raw.tobytes()

    def __iter__(self):
        return iter(self._chunk)


class _FakeNumpy(types.ModuleType):
    """Just enough numpy for the callback's indicator math: np.mean and
    np.sqrt over the powered _MonoColumn, plus the np.int16 dtype token
    passed to astype (which ignores it — quantization happens in
    tobytes)."""

    int16 = types.SimpleNamespace(code="h")

    @staticmethod
    def sqrt(x: float) -> float:
        return math.sqrt(x)

    @staticmethod
    def mean(x) -> float:
        chunk = x._chunk
        return sum(chunk) / len(chunk) if chunk else 0.0


class _FrameReplayStream:
    """sounddevice.InputStream stand-in: replays a WAV in 30 ms frames.

    The capture loop opens it and enters it; __enter__ drives the
    engine's callback with every complete frame of the fixture in
    order. Partial trailing frames are outside the contract —
    production streams deliver complete frames only.
    """

    def __init__(self, samples: list[int], callback) -> None:
        self._samples = samples
        self._callback = callback

    def __enter__(self):
        n = FRAME_SAMPLES
        for i in range(0, len(self._samples) - n + 1, n):
            chunk = self._samples[i : i + n]
            floats = [s / 32768.0 for s in chunk]
            self._callback(_MonoColumn(floats), n, None, None)
        return self

    def __exit__(self, *exc) -> bool:
        return False


def _load_expected() -> dict:
    with EXPECTED.open(encoding="utf-8") as fh:
        return json.load(fh)


def _load_wav(name: str) -> list[int]:
    with wave.open(str(GOLDEN_DIR / f"{name}.wav")) as wf:
        raw = wf.readframes(wf.getnframes())
    ints = array.array("h")
    ints.frombytes(raw)
    return list(ints)


def _engine_settings() -> eng.DictateSettings:
    return eng.DictateSettings(
        language="ru",
        initial_prompt="",
        idle_timeout=0,
        auto_stop_silence=0,  # auto-stop would end the session mid-fixture
        hotkey="<cmd>+<shift>+.",
        trigger="toggle",
        vad_enabled=True,  # so the state machine runs; VAD itself is stubbed
        show_indicator=False,
        idle_visible=False,
        menu_bar=False,
        frame_energy=0.010,
        min_energy=0.008,
        min_utterance=0.25,
    )


def _make_replaying_engine(name: str, stt: _RecordingSTT) -> eng.DictationEngine:
    """Build a real engine and install the replay fakes around it."""
    engine = eng.DictationEngine(
        _engine_settings(), stt, _NullInjector(), _NullIndicator()
    )
    engine._vad = _AlwaysSpeechVAD()  # type: ignore[assignment]

    samples = _load_wav(name)
    replay_mod = types.ModuleType("sounddevice")
    replay_mod.InputStream = lambda **kw: _FrameReplayStream(samples, kw["callback"])
    # After the fake stream's __enter__ replays the fixture, the capture
    # loop's `while self._capturing` would spin forever (our sleep is
    # instant). The first post-replay tick stops the loop instead.
    replay_mod.sleep = lambda ms: setattr(engine, "_capturing", False)
    return engine, replay_mod


def _run_capture(engine: eng.DictationEngine, replay_mod) -> None:
    orig_sd = sys.modules.get("sounddevice")
    orig_np = sys.modules.get("numpy")
    sys.modules["sounddevice"] = replay_mod
    sys.modules["numpy"] = _FakeNumpy("numpy")
    try:
        engine._capturing = True
        engine._capture_loop()
    finally:
        if orig_sd is None:
            sys.modules.pop("sounddevice", None)
        else:
            sys.modules["sounddevice"] = orig_sd
        if orig_np is None:
            sys.modules.pop("numpy", None)
        else:
            sys.modules["numpy"] = orig_np
        engine._capturing = False


def _drive_regions(name: str) -> list[tuple[float, float]]:
    """Replay one fixture; return region (start, close) times in seconds.

    Region times are reconstructed from pure engine bookkeeping at
    enqueue time: the number of buffered 30 ms frames and the total
    frame count fed so far. The enqueue happens inside the callback
    invocation that closed the region, so the closing frame is the
    (frame_index - 1)-th frame.
    """
    stt = _RecordingSTT()
    engine, replay_mod = _make_replaying_engine(name, stt)

    regions: list[tuple[float, float]] = []
    orig_enqueue = engine._enqueue_utterance
    fed_frames = {"n": 0}

    orig_process = engine._process_vad_frames

    def counting_process(pcm: bytes, frame_bytes: int, frame_seconds: float) -> None:
        fed_frames["n"] += len(pcm) // frame_bytes
        orig_process(pcm, frame_bytes, frame_seconds)

    def recording_enqueue() -> None:
        if engine._utterance_buffer:
            n_frames = len(engine._utterance_buffer)
            close_idx = fed_frames["n"] - 1
            start_idx = close_idx - n_frames + 1
            regions.append(
                (start_idx * FRAME_SECONDS, close_idx * FRAME_SECONDS)
            )
        orig_enqueue()

    engine._process_vad_frames = counting_process
    engine._enqueue_utterance = recording_enqueue
    _run_capture(engine, replay_mod)
    return regions


def _drive_buffers(name: str) -> tuple[eng.DictationEngine, list[bytes]]:
    """Replay one fixture; return the engine and its queued buffers.

    The transcribe worker never starts (_start_session isn't called),
    so the queue holds every closed utterance, pre-gate.
    """
    stt = _RecordingSTT()
    engine, replay_mod = _make_replaying_engine(name, stt)
    _run_capture(engine, replay_mod)

    queued: list[bytes] = []
    while not engine._utterance_queue.empty():
        queued.append(engine._utterance_queue.get_nowait())
    return engine, queued


def test_golden_corpus_exists() -> None:
    assert EXPECTED.exists(), "missing tuning/golden/expected.json — run generate.py"
    for name in CASES:
        assert (GOLDEN_DIR / f"{name}.wav").exists(), f"missing {name}.wav"


@pytest.mark.parametrize("name", CASES)
def test_engine_regions_match_expected(name: str) -> None:
    """The engine's real callback must segment the fixture as pinned."""
    expected = _load_expected()[name]
    regions = _drive_regions(name)

    assert len(regions) == len(expected), (
        f"{name}: engine produced {len(regions)} region(s), expected {len(expected)}: "
        f"got {[(round(s, 3), round(e, 3)) for s, e in regions]}"
    )
    for (start, close), exp in zip(regions, expected):
        assert math.isclose(start, exp["start"], rel_tol=0.0, abs_tol=1e-9), (
            f"{name}: region start {start} != expected {exp['start']}"
        )
        assert math.isclose(close, exp["end"], rel_tol=0.0, abs_tol=1e-9), (
            f"{name}: region end {close} != expected {exp['end']} "
            "(start of the closing frame)"
        )


@pytest.mark.parametrize("name", CASES)
def test_energy_gate_verdicts_match_expected(name: str) -> None:
    """Buffers passing the engine's energy gate match the pinned verdicts.

    Evaluates the real gate — _transcribe_and_inject's RMS check against
    the CALIBRATED _effective_min_energy — over each queued buffer. The
    length gate never bites here (every padded buffer is >= 0.81s
    >= min_utterance 0.25s), so the energy gate is the only filter.
    """
    expected = _load_expected()[name]
    engine, queued = _drive_buffers(name)

    passed = sum(1 for buf in queued if eng._rms_int16(buf) >= engine._effective_min_energy)
    expected_pass = sum(1 for r in expected if not r["rejected_by_energy_gate"])

    assert passed == expected_pass, (
        f"{name}: {passed} buffer(s) passed the energy gate, expected {expected_pass} "
        f"(calibrated gate {engine._effective_min_energy:.4f})"
    )


def test_calibration_with_speech_aborts_to_static_gates() -> None:
    """Speech filling the calibration window must not poison the gates.

    speech_during_calibration pins the speech-aware calibration fix: the
    speech frames are excluded from the noise median, fewer than
    _NOISE_MIN_SAMPLES quiet frames remain, and calibration aborts to the
    static gates — so the first word is segmented AND passes the energy
    gate. (Before the fix, the median was measured on speech, the gate rose
    to ~3x the speech RMS, and the first word was silently rejected.)
    """
    engine, queued = _drive_buffers("speech_during_calibration")

    assert engine._noise_calibrated
    assert engine._effective_min_energy == engine.s.min_energy, (
        "a window full of speech must abort calibration to the static "
        "utterance gate — the median may never be measured on speech"
    )
    assert len(queued) == 1
    rms = eng._rms_int16(queued[0])
    assert rms >= engine._effective_min_energy, (
        f"buffer RMS {rms:.4f} should pass the static gate "
        f"{engine._effective_min_energy:.4f} — the first word must survive"
    )


def test_calibration_with_speech_over_noise_excludes_speech() -> None:
    """Mixed windows measure the floor on quiet frames only — and still
    adapt.

    speech_over_noise_in_calibration: fan-level noise followed by speech
    inside the window. The speech frames are excluded from the median, the
    noise frames raise both gates, and the utterance still passes because
    the speech clears the raised gate. A regression to "any speech in the
    window → skip calibration" would fail here (gates would stay static and
    the noise tail would pass too), so this pins the exclusion mechanism
    itself, not just the abort path.
    """
    engine, queued = _drive_buffers("speech_over_noise_in_calibration")

    assert engine._noise_calibrated
    assert engine._effective_min_energy > engine.s.min_energy, (
        "the quiet (noise) frames in the window must still raise the "
        "utterance gate — speech exclusion must not disable adaptation"
    )
    assert len(queued) == 1
    rms = eng._rms_int16(queued[0])
    assert rms >= engine._effective_min_energy, (
        f"buffer RMS {rms:.4f} should pass the raised gate "
        f"{engine._effective_min_energy:.4f}"
    )
