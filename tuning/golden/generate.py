#!/usr/bin/env python3
"""Generate the shared segmentation golden corpus.

Synthesizes deterministic 16 kHz mono s16le WAV fixtures that pin the
utterance-segmentation behavior every whiz implementation must agree on,
and writes ``expected.json`` by running the reference logic (the
Python engine's energy-gate + trailing-silence state machine, with
webrtcvad disabled so no model is needed) over each fixture.

The fixtures live in this repo and are consumed by:
- tests/test_segmentation_golden.py     (Python)
- macos/Tests/WhizAppTests/TuningTests.swift (Swift)

Determinism rules — the corpus must regenerate byte-identically:
- No timestamps, no randomness, no floating-point time. Everything is
  frame-index arithmetic on a fixed sine/click synthesis.
- RMS values are chosen with wide margins against the tuning floors so
  the cases stay meaningful if the floors are retuned.

What the contract covers (and deliberately does not):

expected.json pins TWO things per region:
- ``start`` / ``end``: the speech-region boundaries as produced by the
  energy-gate state machine. ``end`` is the start time of the frame
  that closed the region, so the region spans speech content plus
  utterance_silence. Implementations reconcile their own trailing-
  silence policy against this: Python buffers everything up to ``end``
  plus one frame; Swift trims to ``end - utterance_silence +
  trailing_padding``.
- ``rejected_by_energy_gate``: whether the closed utterance fails the
  whole-buffer RMS gate that runs before transcription (Python:
  ``_transcribe_and_inject``; Swift: ``SessionController.enqueue``).
  The generator computes the verdict under BOTH padding policies
  (Python's full trailing silence, Swift's 0.2s trim) and refuses to
  write the corpus if they disagree — a case where the policies give
  different verdicts needs redesigned amplitudes, not a pinned lie.

It does NOT pin the min-utterance length gate: Python applies it to the
padded buffer (speech + 0.81s mandatory trailing silence, so it can
never reject a silence-closed utterance) while Swift applies it to the
trimmed one — a known, documented divergence. See docs/ARCHITECTURE.md.

Cases:
1. quiet_two_utterances    — 1.2s lead-in, speech/gap/speech. Two
   regions. The lead-in is longer than the calibration window so the
   noise floor is measured on silence, not on speech.
2. speech_during_calibration — speech starts at t=0, filling the
   calibration window. Segmentation keeps it (the bug fixed in the PR
   #1 review dropped these frames entirely), but the poisoned median
   raises the utterance gate to ~3x the speech RMS, so the energy gate
   then REJECTS the first utterance. This is real shared behavior in
   both implementations — a known defect, pinned here as-is; see
   docs/ARCHITECTURE.md ("Known shared defects").
3. speech_late_in_calibration — speech starts at t=0.9s, near the end
   of the 1.0s window. Silence dominates the median, the gates stay at
   their static floors, and the utterance survives — the boundary
   counterpart of case 2.
4. noisy_room — steady noise ABOVE the static frame floor for the whole
   clip. Only the adaptive calibration can reject it. Segmentation
   still opens one spurious region during the window (the static gate
   is in force until calibration applies), which then closes normally
   and is rejected by the energy gate.
5. click_below_min — a loud 0.12s transient, shorter than
   min_utterance. Segmentation opens and closes a region for it
   identically in every implementation. Both length gates then PASS it
   (Python measures the padded 0.93s buffer; Swift measures the trimmed
   0.32s — both >= 0.25): a shared limitation, since the gate was meant
   to reject exactly this. The Python/Swift length-gate divergence
   (padded vs trimmed duration) only bites single-frame blips and is
   documented, not pinned — see the note above.
6. trailing_silence_trim — 0.9s speech followed by >= utterance_silence
   of silence: the region closes at exactly 27 frames (0.81s), and
   the leftover tail opens nothing.
7. gap_below_silence — a 0.78s (26-frame) gap between two phrases:
   below the close threshold, so the two phrases merge into ONE region.

Regenerate after changing tuning/tuning.toml or the segmentation logic:
    python3 tuning/golden/generate.py
"""

from __future__ import annotations

import json
import math
import struct
import wave
from pathlib import Path

SAMPLE_RATE = 16000
SAMPLE_S16_MAX = 32767
SAMPLE_S16_MIN = -32768

HERE = Path(__file__).resolve().parent

# Amplitudes chosen with wide margins against the tuning floors in
# tuning/tuning.toml.
SPEECH_AMP = 0.06          # normalized peak — RMS 0.042, well above every gate
NOISY_FLOOR_AMP = 0.03     # RMS 0.021 — above the 0.010 static floor,
                           # so ONLY calibration can reject it

# From the tuning contract (duplicated here only to synthesize cases;
# pinned against tuning/tuning.toml by tests/test_tuning.py).
UTTERANCE_SILENCE = 0.8
TRAILING_PADDING = 0.2
FRAME_ENERGY_DEFAULT = 0.010
MIN_ENERGY_DEFAULT = 0.008

# The engine's no-VAD frame: 480 samples = 0.03s at 16 kHz.
FRAME_SECONDS = 0.03
FRAME_LEN = 480

# Calibration constants, mirroring engine.py (pinned by tests/test_tuning.py).
CAL_WINDOW = 1.0
FRAME_MULT = 3.5
UTT_MULT = 3.0
MIN_SAMPLES = 5


def _sinusoid_segment(duration_s: float, amp_norm: float, freq: float = 200.0) -> list[int]:
    """A deterministic sine segment scaled to int16 amplitude."""
    n = int(round(SAMPLE_RATE * duration_s))
    scale = amp_norm * SAMPLE_S16_MAX
    return [int(round(scale * math.sin(2.0 * math.pi * freq * i / SAMPLE_RATE))) for i in range(n)]


def _silence_segment(duration_s: float) -> list[int]:
    return [0] * int(round(SAMPLE_RATE * duration_s))


def _click(duration_s: float, amp_norm: float) -> list[int]:
    """A short transient — alternating extremes, like a key click."""
    n = int(round(SAMPLE_RATE * duration_s))
    peak = int(round(amp_norm * SAMPLE_S16_MAX))
    return [peak if i % 2 else -peak for i in range(n)]


def _write_wav(path: Path, samples: list[int]) -> None:
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(struct.pack(f"<{len(samples)}h", *samples))


# --- Reference segmentation (mirrors engine.py's energy state machine) ---

def _rms(samples) -> float:
    if len(samples) == 0:
        return 0.0
    total = sum(v * v for v in samples)
    return (total / len(samples)) ** 0.5 / SAMPLE_S16_MAX


def _frames(samples: list[int]) -> list[list[int]]:
    """Split into FRAME_LEN frames, dropping any trailing partial frame.

    The production engines consume complete fixed-size frames (the
    sounddevice blocksize in Python, whole converted buffers in Swift);
    a partial trailing frame is outside the contract.
    """
    return [
        samples[i : i + FRAME_LEN]
        for i in range(0, len(samples) - FRAME_LEN + 1, FRAME_LEN)
    ]


def reference_speech_regions(
    samples: list[int],
    *,
    frame_energy: float,
    min_energy: float,
) -> list[dict[str, object]]:
    """The reference implementation, transcribed from engine.py.

    Runs the energy-gate state machine frame by frame (webrtcvad OFF —
    secondary VAD is implementation-specific and must not feed the
    golden file):
    - frames below the effective frame gate are silence;
    - speech starts/extends on frames above the gate;
    - utterance_silence of continuous silence closes an utterance, and
      the closing silent frames belong to the buffer (engine.py
      appends the frame BEFORE testing the accumulated silence).

    Calibration follows engine.py exactly: per-frame RMS is collected
    for the first int(cal_window / frame_seconds) + 1 frames, then the
    median raises both gates; frames are NOT dropped during calibration
    and the frame that completes the window is itself measured against
    the static gates.

    Each returned region is {"start", "end", "rejected_by_energy_gate"}:
    - start/end in seconds; end is the start time of the closing frame,
      so the buffered utterance spans [start, end + frame).
    - rejected_by_energy_gate compares the whole-buffer RMS against the
      calibrated utterance gate, and is verified to agree under both
      the full-trailing-silence (Python) and 0.2s-trim (Swift) padding
      policies.
    """
    frames = _frames(samples)

    cal_needed = int(CAL_WINDOW / FRAME_SECONDS) + 1

    eff_frame_gate = frame_energy
    eff_utt_gate = min_energy

    in_speech = False
    silence_frames = 0
    speech_start_frame = 0
    cal_rms: list[float] = []
    calibrated = False
    regions: list[dict[str, object]] = []
    buffer: list[int] = []

    def _energy_gate_verdict(buf: list[int], silent_frames: int) -> bool:
        """True if the utterance fails the RMS gate, checked under both
        padding policies (full trailing silence vs trailing_padding
        trim). Raises instead of pinning a case the two policies
        disagree on."""
        padded_rms = _rms(buf)
        keep = int(TRAILING_PADDING * SAMPLE_RATE)
        drop = max(0, int(silent_frames * FRAME_SECONDS * SAMPLE_RATE) - keep)
        trimmed = buf[: len(buf) - drop] if 0 < drop < len(buf) else buf
        trimmed_rms = _rms(trimmed)
        if (padded_rms < eff_utt_gate) != (trimmed_rms < eff_utt_gate):
            raise SystemExit(
                f"energy-gate verdict differs between padding policies "
                f"(padded {padded_rms:.4f} vs trimmed {trimmed_rms:.4f} "
                f"against gate {eff_utt_gate:.4f}) — redesign this case"
            )
        return padded_rms < eff_utt_gate

    def close_region(idx: int) -> None:
        nonlocal in_speech, silence_frames, buffer
        regions.append(
            {
                "start": round(speech_start_frame * FRAME_SECONDS, 6),
                "end": round(idx * FRAME_SECONDS, 6),
                "rejected_by_energy_gate": _energy_gate_verdict(buffer, silence_frames),
            }
        )
        in_speech = False
        silence_frames = 0
        buffer = []

    for idx, frame in enumerate(frames):
        energy = _rms(frame)
        # Calibration: collect, then apply — but keep processing the
        # frame (engine.py falls through to segmentation during
        # calibration). The frame that completes the window is itself
        # measured against the RAISED gates — engine.py calls
        # _finish_noise_calibration() before the segmentation check,
        # so a noisy window's completing frame already counts as
        # silence (see cases 2 and 4: regions close at 1.77s, one
        # frame earlier than a static-gate reading would give).
        if not calibrated:
            cal_rms.append(energy)
            if len(cal_rms) >= max(cal_needed, MIN_SAMPLES):
                calibrated = True
                s = sorted(cal_rms)
                n = len(s)
                median = s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2
                eff_frame_gate = max(frame_energy, median * FRAME_MULT)
                eff_utt_gate = max(min_energy, median * UTT_MULT)

        if energy >= eff_frame_gate:
            if not in_speech:
                in_speech = True
                speech_start_frame = idx
            silence_frames = 0
            buffer.extend(frame)
        else:
            if in_speech:
                buffer.extend(frame)
                silence_frames += 1
                if silence_frames * FRAME_SECONDS >= UTTERANCE_SILENCE:
                    close_region(idx)

    # Flush at end of input — engine.py's _end_session flushes whatever
    # is buffered. No fixture below ends mid-speech, so this exists for
    # faithfulness, not coverage.
    if in_speech:
        regions.append(
            {
                "start": round(speech_start_frame * FRAME_SECONDS, 6),
                "end": round(len(frames) * FRAME_SECONDS, 6),
                "rejected_by_energy_gate": _energy_gate_verdict(buffer, silence_frames),
            }
        )
    return regions


def main() -> None:
    cases: dict[str, list[int]] = {}

    # 1. Two utterances in a quiet room. The 1.2s lead-in exceeds the
    #    calibration window, so the noise floor is measured on silence.
    cases["quiet_two_utterances"] = (
        _silence_segment(1.2)
        + _sinusoid_segment(1.2, SPEECH_AMP)
        + _silence_segment(1.0)
        + _sinusoid_segment(0.8, SPEECH_AMP)
        + _silence_segment(1.0)
    )

    # 2. Speech starts at t=0 — inside the calibration window. The
    #    region must be segmented (the PR #1 Swift bug dropped it), but
    #    the poisoned median raises the utterance gate above the speech
    #    RMS, so the energy gate rejects the first word. Known shared
    #    defect, pinned as-is — see docs/ARCHITECTURE.md.
    cases["speech_during_calibration"] = (
        _sinusoid_segment(1.0, SPEECH_AMP)
        + _silence_segment(1.5)
    )

    # 3. Speech starts at t=0.9s, near the end of the window: silence
    #    dominates the median, the gates stay static, the word survives.
    cases["speech_late_in_calibration"] = (
        _silence_segment(0.9)
        + _sinusoid_segment(1.0, SPEECH_AMP)
        + _silence_segment(1.0)
    )

    # 4. Noisy room: steady noise above the static frame floor. Only the
    #    adaptive calibration can reject it. One spurious region opens
    #    before the raised gates take effect, then closes and is
    #    rejected by the energy gate.
    cases["noisy_room"] = _sinusoid_segment(4.0, NOISY_FLOOR_AMP, freq=60.0)

    # 5. A loud 0.12s click, shorter than min_utterance. Segmentation
    #    treats it identically everywhere; the length-gate outcome
    #    afterwards diverges (documented, not pinned).
    cases["click_below_min"] = (
        _silence_segment(1.2)
        + _click(0.12, SPEECH_AMP)
        + _silence_segment(1.2)
    )

    # 6. Speech followed by >= utterance_silence of silence: closes at
    #    exactly 27 frames, and the leftover tail opens nothing.
    cases["trailing_silence_trim"] = (
        _silence_segment(1.2)
        + _sinusoid_segment(0.9, SPEECH_AMP)
        + _silence_segment(1.0)
    )

    # 7. A 0.78s gap — one frame short of the close threshold — merges
    #    two phrases into a single region.
    cases["gap_below_silence"] = (
        _silence_segment(1.2)
        + _sinusoid_segment(0.9, SPEECH_AMP)
        + _silence_segment(0.78)
        + _sinusoid_segment(0.9, SPEECH_AMP)
        + _silence_segment(1.0)
    )

    expected: dict[str, list[dict[str, object]]] = {}
    for name, samples in cases.items():
        wav_path = HERE / f"{name}.wav"
        _write_wav(wav_path, samples)
        expected[name] = reference_speech_regions(
            samples, frame_energy=FRAME_ENERGY_DEFAULT, min_energy=MIN_ENERGY_DEFAULT
        )

    (HERE / "expected.json").write_text(
        json.dumps(expected, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Wrote {len(cases)} fixtures + expected.json to {HERE}")


if __name__ == "__main__":
    main()