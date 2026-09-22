#include "tiers.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "audio/wav.hpp"
#include "table.hpp"

namespace mynah::models {

bool gpu_available(bool user_opted_into_discrete) {
    // The same choice the STT makes when it loads, so the benchmark
    // measures turbo on the GPU it will really run on.
    return stt::pick_gpu(user_opted_into_discrete).has_value();
}

std::optional<BenchmarkResult> benchmark(stt::SpeechToText& stt,
                                         const std::string& model,
                                         const std::string& clip_path,
                                         const std::string& language,
                                         bool discrete_gpu) {
    if (!stt.load(model, discrete_gpu)) return std::nullopt;

    std::vector<float> samples;
    try {
        samples = mynah::audio::read_wav_mono_16k(clip_path);
    } catch (const std::exception&) {
        return std::nullopt; // no clip: the caller skips, the CLI says why
    }
    if (samples.empty()) return std::nullopt;

    auto began = std::chrono::steady_clock::now();
    auto text = stt.transcribe(samples.data(), samples.size(), language, "");
    auto ended = std::chrono::steady_clock::now();
    if (!text) return std::nullopt;

    BenchmarkResult result;
    result.model = std::filesystem::path(model).filename().string();
    result.seconds =
        std::chrono::duration<double>(ended - began).count();
    result.text = *text;
    return result;
}

std::string choose_tier(bool gpu_ready, const std::optional<double>& turbo_seconds,
                        const std::optional<double>& small_seconds) {
    // A tier is chosen only on a measurement that MEETS its budget; a
    // candidate that failed to load or run never promotes the tier above
    // it. `base` is the floor — it is always chosen rather than nothing,
    // so a machine too slow for anything still gets a working mynah.
    if (gpu_ready && turbo_seconds && *turbo_seconds <= kGpuBudgetSeconds) return "gpu";
    if (small_seconds && *small_seconds <= kSmallBudgetSeconds) return "small";
    return "base";
}

} // namespace mynah::models