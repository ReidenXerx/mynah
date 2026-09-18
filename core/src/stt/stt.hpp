// mynah::stt — speech-to-text behind the session.
//
// Port of WhisperEngine.swift: whisper.cpp linked in-process (never a
// subprocess per utterance — the model stays warm between utterances),
// with the decode parameters the Python and Swift engines both settled on
// after a year of tuning. Those parameters are the anti-hallucination
// posture: greedy decoding, no carried context, suppressed blanks and
// non-speech tokens, and the two decoder thresholds pinned against
// tuning/tuning.toml (W2-M12).
//
// The interface exists so the session's state machine can be tested
// without a model; the tests substitute fakes.

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace mynah::stt {

class SpeechToText {
public:
    virtual ~SpeechToText() = default;

    // Load the model file. Seconds on a cold start; runs on the session's
    // loader thread, off every other thread. Returns false on failure.
    virtual bool load(const std::filesystem::path& model) = 0;
    virtual bool is_loaded() const = 0;

    // Free the model — the "zero RAM at idle" behaviour. The session calls
    // this after idle_timeout and the engine on destroy.
    virtual void unload() = 0;

    // Transcribe 16 kHz mono samples. language may be "auto". The prompt
    // biases the decoder — it carries the anti-censorship Russian prompt,
    // which is load-bearing: without it Whisper sanitises slang and
    // obscenity rather than transcribing it verbatim. Empty text is a
    // valid answer (silence decoded to nothing).
    virtual std::optional<std::string> transcribe(const float* samples, std::size_t count,
                                                 const std::string& language,
                                                 const std::string& prompt) = 0;
};

// whisper.cpp on the compute backend the platform gives it (Metal on
// Apple Silicon).
std::unique_ptr<SpeechToText> make_whisper();

} // namespace mynah::stt