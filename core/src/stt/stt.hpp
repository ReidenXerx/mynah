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
#include <vector>

namespace mynah::stt {

class SpeechToText {
public:
    virtual ~SpeechToText() = default;

    // Load the model file. Seconds on a cold start; runs on the session's
    // loader thread, off every other thread. Returns false on failure.
    // `discrete_gpu` is config `gpu`: whether a discrete GPU may run it
    // (see choose_gpu); ignored on macOS, where it is always Metal (M4).
    virtual bool load(const std::filesystem::path& model, bool discrete_gpu) = 0;
    virtual bool is_loaded() const = 0;

    // Whether load() with the same `discrete_gpu` would run on a GPU — what
    // an empty `model` resolves to depends on it (models::resolve).
    virtual bool gpu_available(bool discrete_gpu) const = 0;

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

// The GPU policy on Linux (docs/ENGINE-MIGRATION.md, "Linux model tiers"):
// an integrated GPU is used automatically; a discrete one only when the
// user opted in (config `gpu`), and then in preference to the integrated
// one. `gpus` lists the GPU-type devices in ggml's registry order; the
// result indexes into it — which is how whisper_context_params::gpu_device
// counts — or is empty for the CPU.
enum class GpuKind { Discrete, Integrated };
std::optional<int> choose_gpu(const std::vector<GpuKind>& gpus, bool discrete_allowed);

// choose_gpu over the devices this process can see, with their names;
// empty for the CPU. Registers ggml's backends first if nothing has.
struct GpuChoice {
    int index = 0;      // for whisper_context_params::gpu_device
    std::string name;   // ggml's device description, for logs and setup
    GpuKind kind = GpuKind::Integrated;
};
std::optional<GpuChoice> pick_gpu(bool discrete_allowed);

} // namespace mynah::stt