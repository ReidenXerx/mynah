// mynah::models — the download table (P8) and the Linux tiers (M5).
//
// P8: the core PUBLISHES each model's URL, size and SHA-256; the front end
// downloads (the Linux CLI with libcurl, the Mac app with URLSession) and
// verifies the hash. The core stays free of network code.
//
// Every URL names a fixed revision of its repository, never `resolve/main`:
// whatever is behind `main` tomorrow is not what anyone reviewed, and
// whisper.cpp parses the file as a native binary format. Each SHA-256 is
// the one HuggingFace publishes for that file at that revision (its LFS
// object id); small and base also match the ones pinned on `main` in
// b7d498d. A wrong hash bricks every download, so a revision bump re-reads
// them from the publisher rather than from a downloaded copy.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace mynah::models {

struct ModelInfo {
    std::string_view filename;   // ggml-large-v3-turbo.bin
    std::string_view alias;       // large-v3-turbo
    std::string_view url;         // where the front end downloads it from
    std::uint64_t approximate_bytes; // exact at the pinned revision; progress, disk warnings
    std::string_view sha256;      // lowercase hex, the publisher's (see above)
};

// The models the Linux tiers need (docs/ENGINE-MIGRATION.md, "Linux model
// tiers"): turbo for `gpu`, small and base for the CPU tiers, plus the
// Silero VAD. macOS needs only turbo + Silero (M4) and fetches them from
// the same table.
//
// ggerganov/whisper.cpp at 5359861c (the revision `main` pinned in b7d498d);
// ggml-org/whisper-vad at 9ffd54a1 (2026-09-23).
inline constexpr ModelInfo kTurbo{
    "ggml-large-v3-turbo.bin", "large-v3-turbo",
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/"
    "5359861c739e955e79d9a303bcbc70fb988958b1/ggml-large-v3-turbo.bin",
    1'624'555'275, "1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69"};
inline constexpr ModelInfo kSmall{
    "ggml-small.bin", "small",
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/"
    "5359861c739e955e79d9a303bcbc70fb988958b1/ggml-small.bin",
    487'601'967, "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b"};
inline constexpr ModelInfo kBase{
    "ggml-base.bin", "base",
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/"
    "5359861c739e955e79d9a303bcbc70fb988958b1/ggml-base.bin",
    147'951'465, "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe"};
inline constexpr ModelInfo kSileroVad{
    "ggml-silero-v5.1.2.bin", "silero-v5.1.2",
    "https://huggingface.co/ggml-org/whisper-vad/resolve/"
    "9ffd54a1e1ee413ddf265af9913beaf518d1639b/ggml-silero-v5.1.2.bin",
    885'098, "29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf"};

// Where downloads land: first in the core's search order
// (models::search_directories), so what a front end fetches, resolve finds.
std::string download_dir();

// The model a tier name refers to: "gpu" -> turbo, "small" -> small,
// "base" -> base. Unknown -> nullptr.
const ModelInfo* model_for_tier(std::string_view tier);

// Info by alias or filename ("small", "ggml-small.bin", "turbo"), or nullptr.
const ModelInfo* find(std::string_view name);

} // namespace mynah::models