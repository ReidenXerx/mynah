#include "table.hpp"

#include <cstdlib>
#include <filesystem>

namespace mynah::models {

namespace {

bool matches(const ModelInfo& info, std::string_view name) {
    if (name == info.filename || name == info.alias) return true;
    // "ggml-small.bin" for alias "small" — the form the settings UIs show.
    return name == "ggml-" + std::string(info.alias) + ".bin";
}

} // namespace

std::string download_dir() {
    const char* home = std::getenv("HOME");
    std::filesystem::path base =
        home ? std::filesystem::path(home) : std::filesystem::path(".");
    return (base / ".cache" / "whisper").string();
}

const ModelInfo* model_for_tier(std::string_view tier) {
    if (tier == "gpu") return &kTurbo;
    if (tier == "small") return &kSmall;
    if (tier == "base") return &kBase;
    return nullptr;
}

const ModelInfo* find(std::string_view name) {
    for (const ModelInfo* info : {&kTurbo, &kSmall, &kBase, &kSileroVad})
        if (matches(*info, name)) return info;
    return nullptr;
}

} // namespace mynah::models