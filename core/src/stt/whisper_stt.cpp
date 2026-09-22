#include "stt.hpp"

#include <mutex>
#include <thread>
#include <vector>

#include <ggml-backend.h>
#include <whisper.h>

#include "backends.hpp"

#include "tuning/constants.hpp"

namespace mynah::stt {

namespace {


class WhisperStt final : public SpeechToText {
public:
    ~WhisperStt() override { unload(); }

    bool load(const std::filesystem::path& model, bool discrete_gpu) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (context_) return true;
        mynah::ggml::register_backends_once();

        whisper_context_params params = whisper_context_default_params();
#if defined(__APPLE__)
        // Metal on Apple Silicon — the whole reason for a GPU-capable
        // runtime (M4: always, no tiers).
        (void)discrete_gpu;
        params.use_gpu = true;
#else
        // Chosen, not whisper.cpp's default of "the first GPU it finds":
        // with Vulkan that can be the discrete GPU, woken for every
        // sentence even with `gpu` off.
        std::optional<GpuChoice> gpu = pick_gpu(discrete_gpu);
        params.use_gpu = gpu.has_value();
        params.gpu_device = gpu ? gpu->index : 0;
#endif
        // flash_attn: faster and slightly more accurate with it.
        params.flash_attn = true;

        context_ = whisper_init_from_file_with_params(model.string().c_str(), params);
        return context_ != nullptr;
    }

    bool is_loaded() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return context_ != nullptr;
    }

    bool gpu_available(bool discrete_gpu) const override {
#if defined(__APPLE__)
        (void)discrete_gpu;
        return true; // Metal
#else
        return pick_gpu(discrete_gpu).has_value();
#endif
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

std::optional<int> choose_gpu(const std::vector<GpuKind>& gpus, bool discrete_allowed) {
    auto first = [&gpus](GpuKind kind) -> std::optional<int> {
        for (std::size_t i = 0; i < gpus.size(); ++i)
            if (gpus[i] == kind) return int(i);
        return std::nullopt;
    };
    if (discrete_allowed)
        if (auto discrete = first(GpuKind::Discrete)) return discrete;
    return first(GpuKind::Integrated);
}

std::optional<GpuChoice> pick_gpu(bool discrete_allowed) {
    mynah::ggml::register_backends_once();
    // The same walk as whisper_backend_init_gpu: GPU and IGPU devices, in
    // registry order, counted together — so the index means the same there.
    std::vector<GpuKind> kinds;
    std::vector<ggml_backend_dev_t> devices;
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t device = ggml_backend_dev_get(i);
        switch (ggml_backend_dev_type(device)) {
        case GGML_BACKEND_DEVICE_TYPE_GPU: kinds.push_back(GpuKind::Discrete); break;
        case GGML_BACKEND_DEVICE_TYPE_IGPU: kinds.push_back(GpuKind::Integrated); break;
        default: continue;
        }
        devices.push_back(device);
    }
    std::optional<int> index = choose_gpu(kinds, discrete_allowed);
    if (!index) return std::nullopt;
    GpuChoice choice;
    choice.index = *index;
    choice.name = ggml_backend_dev_description(devices[std::size_t(*index)]);
    choice.kind = kinds[std::size_t(*index)];
    return choice;
}

} // namespace mynah::stt