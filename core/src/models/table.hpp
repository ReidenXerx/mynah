// mynah::models — the download table (P8) and the Linux tiers (M5).
//
// P8: the core PUBLISHES each model's URL, size and SHA-256; the front end
// downloads (the Linux CLI with libcurl, the Mac app with URLSession) and
// verifies the hash. The core stays free of network code.
//
// The SHA-256 fields are empty until each file has been verified against
// its publisher by hand; the downloader enforces the hash when present and
// falls back to the HTTP status + size otherwise. A fabricated hash would
// be worse than none: a wrong hash bricks every download.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace mynah::models {

struct ModelInfo {
    std::string_view filename;   // ggml-large-v3-turbo.bin
    std::string_view alias;       // large-v3-turbo
    std::string_view url;         // where the front end downloads it from
    std::uint64_t approximate_bytes; // for progress bars and disk warnings
    std::string_view sha256;      // empty until verified (see above)
};

// The models the Linux tiers need (docs/ENGINE-MIGRATION.md, "Linux model
// tiers"): turbo for `gpu`, small and base for the CPU tiers, plus the
// Silero VAD. macOS needs only turbo + Silero (M4) and fetches them from
// the same table.
inline constexpr ModelInfo kTurbo{
    "ggml-large-v3-turbo.bin", "large-v3-turbo",
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin",
    1'620'000'000, ""};
inline constexpr ModelInfo kSmall{
    "ggml-small.bin", "small",
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin",
    466'000'000, ""};
inline constexpr ModelInfo kBase{
    "ggml-base.bin", "base",
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin",
    142'000'000, ""};
inline constexpr ModelInfo kSileroVad{
    "ggml-silero-v5.1.2.bin", "silero-v5.1.2",
    "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin",
    1'000'000, ""};

// Where downloads land: first in the core's search order
// (models::search_directories), so what a front end fetches, resolve finds.
std::string download_dir();

// The model a tier name refers to: "gpu" -> turbo, "small" -> small,
// "base" -> base. Unknown -> nullptr.
const ModelInfo* model_for_tier(std::string_view tier);

// Info by alias or filename ("small", "ggml-small.bin", "turbo"), or nullptr.
const ModelInfo* find(std::string_view name);

} // namespace mynah::models