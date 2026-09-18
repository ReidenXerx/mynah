#include "stt.hpp"

#include <mutex>
#include <thread>

#include <ggml-backend.h>
#include <whisper.h>

#include "tuning/constants.hpp"

namespace mynah::stt {

namespace {

// See vad/silero.cpp for why this must run before any whisper_*_init_*:
// an unregistered backend hits GGML_ASSERT, which aborts the process.
void register_ggml_backends_once() {
    static bool registered = [] {
        ggml_backend_load_all();
        return true;
    }();
    (void)registered;
}

class WhisperStt final : public SpeechToText {
public:
    ~WhisperStt() override { unload(); }

    bool load(const std::filesystem::path& model) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (context_) return true;
        register_ggml_backends_once();

        whisper_context_params params = whisper_context_default_params();
        // Metal on Apple Silicon — the whole reason for a GPU-capable
        // runtime. flash_attn: faster and slightly more accurate with it.
        params.use_gpu = true;
        params.flash_attn = true;

        context_ = whisper_init_from_file_with_params(model.string().c_str(), params);
        return context_ != nullptr;
    }

    bool is_loaded() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return context_ != nullptr;
    }

    void unload() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (context_) {
            whisper_free(context_);
            context_ = nullptr;
        }
    }

    std::optional<std::string> transcribe(const float* samples, std::size_t count,
                                          const std::string& language,
                                          const std::string& prompt) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_) return std::nullopt;
        if (count == 0) return std::string();

        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        unsigned cores = std::thread::hardware_concurrency();
        params.n_threads = int(cores > 2 ? cores - 2 : 1);
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.print_special = false;
        params.translate = false;
        // Utterances are segmented before they get here, so each call is
        // one self-contained chunk; carrying decoder context across them
        // lets a hallucination in one utterance seed the next.
        params.no_context = true;
        params.no_timestamps = true;
        params.suppress_blank = true;
        // Suppress non-speech tokens: [MUSIC], [SOUND] and similar. A cheap
        // extra layer under the hallucination filter.
        params.suppress_nst = true;
        // The decoder anti-hallucination thresholds (W2-M12), pinned
        // against tuning/tuning.toml — both stricter than whisper.cpp's
        // defaults (0.6 / -1.0): skip segments the model is even
        // moderately confident are silence, and reject low-confidence
        // decodes.
        params.no_speech_thold = constants::whisper_no_speech_threshold;
        params.logprob_thold = constants::whisper_logprob_threshold;

        params.language = language.c_str();
        // whisper_full does not copy these, and both must outlive the
        // call — members of this object, which outlives it.
        prompt_ = prompt;
        params.initial_prompt = prompt_.empty() ? nullptr : prompt_.c_str();

        if (whisper_full(context_, params, samples, int(count)) != 0) return std::nullopt;

        std::string text;
        int segments = whisper_full_n_segments(context_);
        for (int i = 0; i < segments; ++i) {
            const char* segment = whisper_full_get_segment_text(context_, i);
            if (segment) text += segment;
        }
        // Trim leading/trailing whitespace, as WhisperEngine.swift does —
        // the spacing rule and the filter both work on trimmed text.
        std::size_t begin = text.find_first_not_of(" \t\n\r");
        if (begin == std::string::npos) return std::string();
        std::size_t end = text.find_last_not_of(" \t\n\r");
        return text.substr(begin, end - begin + 1);
    }

private:
    mutable std::mutex mutex_; // is_loaded() is a const reader
    whisper_context* context_ = nullptr;
    std::string prompt_; // backing store for whisper_full's borrowed pointer
};

} // namespace

std::unique_ptr<SpeechToText> make_whisper() {
    return std::make_unique<WhisperStt>();
}

} // namespace mynah::stt