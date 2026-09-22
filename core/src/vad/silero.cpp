#include "vad.hpp"

#include <mutex>
#include <thread>

#include <ggml-backend.h>
#include <whisper.h>

#include "stt/backends.hpp"

namespace mynah::vad {

namespace {


class SileroVad final : public VoiceActivity {
public:
    ~SileroVad() override { unload(); }

    bool load(const std::filesystem::path& model) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (context_) return true;
        mynah::ggml::register_backends_once();

        whisper_vad_context_params params = whisper_vad_default_context_params();
        int cores = int(std::thread::hardware_concurrency());
        params.n_threads = cores > 2 ? cores - 2 : 1;
        params.use_gpu = false;

        context_ = whisper_vad_init_from_file_with_params(model.string().c_str(), params);
        return context_ != nullptr;
    }

    bool is_loaded() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return context_ != nullptr;
    }

    void unload() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (context_) {
            whisper_vad_free(context_);
            context_ = nullptr;
        }
    }

    bool contains_speech(const float* samples, std::size_t count) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Fail open: not loaded, nothing to ask, empty input.
        if (!context_ || count == 0) return true;

        whisper_vad_params params = whisper_vad_default_params();
        // Silero's own default; raise if noise gets through, lower if
        // quiet speech is being dropped.
        params.threshold = 0.5f;
        // The utterance is already segmented by the energy gates, so this
        // is a yes/no question about one clip — the durations only need to
        // be permissive enough not to discard a short word.
        params.min_speech_duration_ms = 60;
        params.min_silence_duration_ms = 100;
        params.speech_pad_ms = 0;

        whisper_vad_segments* segments = whisper_vad_segments_from_samples(
            context_, params, samples, int(count));
        if (!segments) return true; // fail open
        int n = whisper_vad_segments_n_segments(segments);
        whisper_vad_free_segments(segments);
        return n > 0;
    }

private:
    mutable std::mutex mutex_; // is_loaded() is a const reader
    whisper_vad_context* context_ = nullptr;
};

class AlwaysSpeech final : public VoiceActivity {
public:
    bool load(const std::filesystem::path&) override { return true; }
    bool is_loaded() const override { return true; }
    void unload() override {}
    bool contains_speech(const float*, std::size_t) override { return true; }
};

} // namespace

std::unique_ptr<VoiceActivity> make_silero() {
    return std::make_unique<SileroVad>();
}

std::unique_ptr<VoiceActivity> make_always_speech() {
    return std::make_unique<AlwaysSpeech>();
}

} // namespace mynah::vad