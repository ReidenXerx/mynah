// mynah::models — locating the ggml model files.
//
// Port of WhisperModel.swift (and mynah/models.py's resolve before it), so
// the same `model` value means the same model on every platform. Preference
// order, search directories, and the three alias forms are pinned by
// core/tests/test_models.cpp — the twin of
// macos/Tests/MynahAppTests/AliasResolutionTests.swift.
//
// Linux model tiers (M5) do not exist yet; they arrive in Phase 3 and grow
// into resolve() without changing what a configured path means.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mynah::models {

// NS-15: unquantized first within each class, quantized only when its own
// class is absent, tiny excluded (useless quality; still loads when
// configured explicitly).
inline constexpr const char* kPreference[] = {
    "ggml-large-v3-turbo.bin",
    "ggml-large-v3-turbo-q8_0.bin",
    "ggml-large-v3-turbo-q5_0.bin",
    "ggml-large-v3.bin",
    "ggml-large-v3-q5_0.bin",
    "ggml-medium.bin",
    "ggml-medium-q5_0.bin",
    "ggml-small.bin",
    "ggml-small-q5_0.bin",
    "ggml-base.bin",
    "ggml-base-q5_0.bin",
};

// The conventional whisper.cpp locations, most user-specific first:
// $MYNAH_MODEL_DIR if set, then ~/.cache/whisper (where the macOS
// downloader writes, so what a front end fetches the core finds), then the
// platform's own — the Library paths on macOS, and on Linux the ones
// mynah/providers/linux_stt.py uses, including the distro packages'
// /usr/share/whisper.cpp.
std::vector<std::filesystem::path> search_directories();

// `ggml-large-v3-turbo-q5_0.bin` -> `large-v3-turbo-q5_0`.
std::string alias_from_filename(const std::string& name);

// Resolve the model to load. An explicit configured value wins in order:
//   1. an existing file path (absolute or ~-relative)
//   2. a bare filename present in a search directory
//   3. an alias: full (`large-v3-turbo-q5_0`), short (`turbo`, only when
//      it matches exactly one model — ambiguity resolves to nothing), or
//      prefix (`large-v3`, picking the best on-disk variant by preference)
// Empty configured walks the preference list. Returns an empty path when
// nothing resolves — the session turns that into a `no_model` problem.
//
// `dirs` lets the tests point resolution at a temp directory; production
// callers use the default.
std::filesystem::path resolve(const std::string& configured,
                             const std::vector<std::filesystem::path>& dirs);

// The Silero VAD model, v5.1.2 first for whisper-cli compatibility.
std::filesystem::path resolve_vad(const std::vector<std::filesystem::path>& dirs);

} // namespace mynah::models