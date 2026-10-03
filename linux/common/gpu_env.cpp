#include "gpu_env.hpp"

#include <fnmatch.h>

#include <cstdio>
#include <cstdlib>
#include <functional>

#include "config/config.hpp"

namespace mynah::gpu_env {

namespace {

constexpr const char* kVariable = "VK_LOADER_DRIVERS_DISABLE";
bool g_was_hidden = false;
bool g_unhidden = false;

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// Each non-empty pattern of a comma-separated list, trimmed.
void for_each_pattern(const std::string& patterns, const std::function<void(const std::string&)>& fn) {
    std::size_t begin = 0;
    while (begin <= patterns.size()) {
        std::size_t end = patterns.find(',', begin);
        if (end == std::string::npos) end = patterns.size();
        const std::string pattern = trimmed(patterns.substr(begin, end - begin));
        if (!pattern.empty()) fn(pattern);
        begin = end + 1;
    }
}

bool matches(const std::string& pattern, const std::string& manifest) {
    return fnmatch(pattern.c_str(), manifest.c_str(), 0) == 0;
}

} // namespace

std::string without_matching(const std::string& patterns, const std::string& manifest) {
    std::string kept;
    for_each_pattern(patterns, [&](const std::string& pattern) {
        if (matches(pattern, manifest)) return;
        if (!kept.empty()) kept += ',';
        kept += pattern;
    });
    return kept;
}

void apply(bool discrete_gpu) {
    const char* value = std::getenv(kVariable);
    if (!value) return;
    const std::string patterns = value;
    for_each_pattern(patterns, [](const std::string& pattern) {
        if (matches(pattern, kNvidiaManifest)) g_was_hidden = true;
    });
    if (!g_was_hidden || !discrete_gpu) return;
    const std::string kept = without_matching(patterns, kNvidiaManifest);
    if (kept.empty()) unsetenv(kVariable);
    else setenv(kVariable, kept.c_str(), 1);
    g_unhidden = true;
    std::fprintf(stderr,
                 "mynah: your session hides the NVIDIA GPU from apps (%s=%s); "
                 "mynah uses it anyway, for itself only (gpu = on)\n",
                 kVariable, patterns.c_str());
}

void apply_configured() { apply(config::load().gpu); }

bool was_hidden() { return g_was_hidden; }
bool unhidden() { return g_unhidden; }

} // namespace mynah::gpu_env
