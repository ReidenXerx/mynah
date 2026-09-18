// mynah::vad — per-utterance voice activity detection.
//
// The energy gates in segment/ decide WHEN an utterance begins and ends —
// they are cheap enough to run on every frame, and also crude: a fan, a
// door, or a keyboard clack passes just as readily as a voice. That is
// what feeds Whisper the near-silent noise it turns into subtitle-credit
// hallucinations. Silero answers the actual question — "is this a human
// voice?" — from the shape of the sound rather than its volume, so the
// split is:
//
//   energy gates: when an utterance begins and ends
//   Silero (here): does it contain speech at all
//
// P3: Silero per utterance through whisper.cpp's whisper_vad_* API, as
// the Swift SileroVAD.swift does. Fail-open everywhere — a VAD that cannot
// run must not silently swallow utterances; the worst case is the
// loudness-only behaviour, not a mute engine.

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace mynah::vad {

// Interface for the session; the tests substitute fakes.
class VoiceActivity {
public:
    virtual ~VoiceActivity() = default;

    // Load the model. Returns false on failure (missing/corrupt file) —
    // the caller then treats the detector as degraded and fails open.
    virtual bool load(const std::filesystem::path& model) = 0;

    virtual bool is_loaded() const = 0;
    virtual void unload() = 0;

    // Whether 16 kHz mono samples contain any speech. Must return true
    // when the detector is unavailable: fail open.
    virtual bool contains_speech(const float* samples, std::size_t count) = 0;
};

// Silero through whisper.cpp's bundled VAD (CPU only — the model is tiny
// and runs in well under a millisecond; dispatching it to a GPU would cost
// more in setup than it saves and would contend with Whisper).
std::unique_ptr<VoiceActivity> make_silero();

// Always-speech stub for tests and the golden corpus driver: the golden
// contract excludes secondary VAD entirely.
std::unique_ptr<VoiceActivity> make_always_speech();

} // namespace mynah::vad