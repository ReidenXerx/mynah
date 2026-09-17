// mynah::constants — the segmentation pipeline's compiled-in constants.
//
// tuning/tuning.toml is the shared contract: Python (tests/test_tuning.py),
// Swift (macos/Tests/MynahAppTests/TuningTests.swift) and now the C++ core
// (core/tests/test_constants.cpp) each hardcode these values in their own
// language and pin them against that file. It is deliberately NOT read at
// runtime. A value changed there must be changed here in the same commit, or
// the pins fail — that is the point.
//
// Values not in tuning.toml (spectrum, merge limits, the default prompt) are
// ported from mynah/engine.py and TranscriptFilter.swift, and say so.

#pragma once

namespace mynah::constants {

// --- Segmentation (pinned: utterance_silence, trailing_padding) ---

// Seconds of continuous silence that ends an utterance within a session.
// Shorter = snappier transcription but may cut off slow speech; longer =
// more natural but adds latency before text appears.
inline constexpr double utterance_silence = 0.8;

// Seconds of trailing silence kept on the utterance when it is closed. The
// core applies Swift's policy (P2): trailing silence is trimmed to this
// before the min-utterance gate, so less near-silence reaches Whisper.
inline constexpr double trailing_padding = 0.2;

// --- Adaptive noise calibration (pinned: noise_*) ---
//
// Measure ambient noise at session start and raise the energy gates
// proportionally: the static floors in the config stay as floors, a noisy
// room gets higher gates. The median's contribution is capped at
// calibration_speech_floor (M13): a calibrated gate can never demand speech
// louder than speech.

inline constexpr double noise_calibration_seconds = 1.0;
// Frame gate ends up ~11 dB above the measured noise floor.
inline constexpr double noise_frame_multiplier = 3.5;
// Utterance gate ends up ~10 dB above the measured noise floor.
inline constexpr double noise_utterance_multiplier = 3.0;
// Calibration needs at least this many QUIET frames to be trusted; fewer
// aborts to the static gates for the session.
inline constexpr int noise_min_samples = 5;

// Calibration frames at or above this RMS are speech, not noise — excluded
// from the median regardless of the gates in force (the legacy measured
// per-frame floor, 0.03 ~= -30 dB, between real MacBook-cooler noise ~0.02
// and quiet speech ~0.04). Also CAPS the calibrated gates (see above).
inline constexpr double calibration_speech_floor = 0.03;

// --- Whisper decoder anti-hallucination thresholds ---
//
// Pinned (whisper_no_speech_threshold, whisper_logprob_threshold); applied
// at decode time, deliberately stricter than Whisper's defaults (0.6 /
// -1.0). Float because whisper.cpp's API takes floats — the pins compare in
// float space, exactly as TuningTests.swift does.
inline constexpr float whisper_no_speech_threshold = 0.35f;
inline constexpr float whisper_logprob_threshold = -0.5f;

// --- Session merge (ported from mynah/engine.py, P4) ---
//
// How much audio one transcription may merge: Whisper's encoder works on a
// 30-second window, and past it the merge stops paying for itself.
inline constexpr double merge_limit_seconds = 20.0;

// --- Spectrum (ported from mynah/engine.py, P4: 12 bands everywhere) ---
//
// Log-spaced across the range speech actually occupies: below 80 Hz is room
// rumble and a desk being knocked, above 5 kHz there is almost nothing left
// of a voice. Divides band magnitudes so a normal speaking voice lands near
// the top of the meter (measured: band magnitudes run 0.001..0.05 with a
// 75th percentile of 0.026).
inline constexpr int spectrum_bands = 12;
inline constexpr double spectrum_low_hz = 80.0;
inline constexpr double spectrum_high_hz = 5000.0;
inline constexpr double spectrum_reference = 0.03;

// --- Default initial prompt (ported from mynah/engine.py) ---
//
// A Russian sentence in informal/jargon register that biases Whisper away
// from self-censoring obscenity/slang: the model treats the prompt as prior
// context, so seeing informal Russian makes it likelier to reproduce
// informal Russian verbatim. The anti-censorship behaviour has never been
// validated under whisper.cpp — the Phase 0 recognition test set does that
// before Phase 2 relies on it.
inline constexpr const char *default_russian_prompt =
    "Это разговорная запись с неформальной лексикой, сленгом и матом. "
    "Запиши всё как есть, без цензуры: пиздец, охуенно, хуйня, ебать, "
    "заебись, бля, сука, хуй, пизда, мудак.";

} // namespace mynah::constants