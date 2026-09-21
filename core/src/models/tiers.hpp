// mynah::models — the Linux tiers (M5) and the benchmark that picks one.
//
// **Benchmark, not guesswork**: on first start with `model` unset, the CLI
// transcribes a bundled real-speech clip with each candidate tier and picks
// by measured seconds per utterance. `model` in the config always overrides
// — a configured value never triggers a benchmark.
//
// Budgets are starting points from the migration plan (Phase 0 was to
// replace them with measurements; it was skipped, so they stay tunable
// here until the recognition work lands real numbers).
//
// GPU policy: the package builds the Vulkan backend, but a *discrete* GPU
// only runs when the user opted in (config `gpu = true`); an integrated one
// is used automatically. Waking a discrete GPU per sentence stays the
// owner's choice — a dGPU spins its fans for a two-word utterance.

#pragma once

#include <optional>
#include <string>

#include "stt/stt.hpp"

namespace mynah::models {

// Seconds per utterance a tier must beat to be chosen.
inline constexpr double kGpuBudgetSeconds = 2.0;  // turbo, GPU
inline constexpr double kSmallBudgetSeconds = 5.0; // small, CPU

// Whether a GPU backend is available for the `gpu` tier: an integrated
// GPU counts always, a discrete one only when the user opted in
// (config `gpu`). Evaluated through ggml's device registry, which is
// populated by ggml_backend_load_all().
bool gpu_available(bool user_opted_into_discrete);

struct BenchmarkResult {
    std::string model;      // filename of the model measured
    double seconds = 0.0;  // wall-clock seconds the clip took
    std::string text;       // what it transcribed (sanity, not asserted)
};

// Time one candidate against the clip. The STT is loaded with `model`
// first; a load failure returns nullopt (the caller skips the candidate).
std::optional<BenchmarkResult> benchmark(stt::SpeechToText& stt,
                                          const std::string& model,
                                          const std::string& clip_path,
                                          const std::string& language);

// The tier the measurements pick: `gpu` if the GPU is there and turbo
// meets its budget; else `small` if it meets its budget; else `base`.
// Candidates whose measurement is missing fall to the slower-but-safe
// tier below them.
std::string choose_tier(bool gpu_ready, const std::optional<double>& turbo_seconds,
                        const std::optional<double>& small_seconds);

} // namespace mynah::models