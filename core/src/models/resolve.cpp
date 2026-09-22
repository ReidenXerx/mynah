#include "resolve.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

namespace mynah::models {

namespace {

bool is_file(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

std::filesystem::path home() {
    const char* home_env = std::getenv("HOME");
    return home_env ? std::filesystem::path(home_env) : std::filesystem::path(".");
}

std::string expand_tilde(const std::string& value) {
    if (value.empty() || value[0] != '~') return value;
    if (value.size() == 1) return home().string();
    if (value[1] == '/') return (home() / value.substr(2)).string();
    return value; // ~user paths are rare; left as-is and not found
}

// `large-v3-turbo-q5_0` -> "turbo"; anything else maps to itself.
std::string short_alias(const std::string& alias) {
    return alias.find("turbo") != std::string::npos ? "turbo" : alias;
}

struct Discovered {
    std::string alias;
    std::filesystem::path path;
};

// Scan the search directories for ggml-*.bin, first directory wins per
// alias, sorted by alias.
std::vector<Discovered> discover(const std::vector<std::filesystem::path>& dirs) {
    std::map<std::string, std::filesystem::path> found;
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec) || ec) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            const std::string name = entry.path().filename().string();
            if (name.rfind("ggml-", 0) != 0) continue;
            if (name.size() < 5 || name.substr(name.size() - 4) != ".bin") continue;
            if (!is_file(entry.path())) continue;
            found.emplace(alias_from_filename(name), entry.path()); // first wins
        }
    }
    std::vector<Discovered> out;
    out.reserve(found.size());
    for (auto& [alias, path] : found) out.push_back(Discovered{alias, path});
    return out;
}

} // namespace

std::vector<std::filesystem::path> search_directories() {
    std::filesystem::path home_path = home();
    std::vector<std::filesystem::path> dirs;

    // MYNAH_MODEL_DIR wins, as it does in mynah/providers/linux_stt.py: a
    // model kept somewhere else (a shared disk, a test fixture) is found
    // without moving it into a cache directory.
    if (const char* override_dir = std::getenv("MYNAH_MODEL_DIR"))
        if (*override_dir) dirs.emplace_back(override_dir);

    // Shared by every platform: what the macOS downloader writes and what
    // whisper.cpp's own scripts use.
    dirs.push_back(home_path / ".cache" / "whisper");

#if defined(__APPLE__)
    dirs.push_back(home_path / "Library/Application Support/com.unspoken.app/WhisperModels");
    dirs.push_back(home_path / "Library/Caches/whisper");
    dirs.push_back("/usr/local/share/whisper");
    dirs.push_back("/opt/homebrew/share/whisper");
    dirs.push_back("/usr/share/whisper");
#else
    // Linux, matching MODEL_DIRS in mynah/providers/linux_stt.py so a
    // model the Python engine found is still found after the cutover —
    // including the distro packages' own locations (pacman's whisper.cpp
    // installs into /usr/share/whisper.cpp).
    dirs.push_back(home_path / ".local/share/mynah/models");
    dirs.push_back(home_path / ".cache/whisper.cpp");
    dirs.push_back("/usr/share/whisper.cpp/models");
    dirs.push_back("/usr/share/whisper.cpp");
    dirs.push_back("/usr/local/share/whisper");
    dirs.push_back("/usr/share/whisper");
#endif
    return dirs;
}

std::string alias_from_filename(const std::string& name) {
    std::string base = name;
    if (base.rfind("ggml-", 0) == 0) base.erase(0, 5);
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".bin") == 0)
        base.erase(base.size() - 4);
    return base;
}

std::filesystem::path resolve(const std::string& configured,
                              const std::vector<std::filesystem::path>& dirs,
                              bool gpu_ready) {
    if (!configured.empty()) {
        std::string expanded = expand_tilde(configured);
        std::filesystem::path direct(expanded);
        if (is_file(direct)) return direct;
        for (const auto& dir : dirs) {
            std::filesystem::path candidate = dir / expanded;
            if (is_file(candidate)) return candidate;
        }
        // Alias resolution: full, then short, then prefix.
        std::vector<Discovered> found = discover(dirs);
        if (found.empty()) return {};
        std::string wanted = alias_from_filename(expanded);

        // Exact alias match.
        for (const auto& model : found)
            if (model.alias == wanted) return model.path;

        // Short alias: "turbo" collapses every turbo variant; it resolves
        // only on a UNIQUE match, as Python does.
        std::vector<const Discovered*> short_matches;
        for (const auto& model : found)
            if (short_alias(model.alias) == short_alias(wanted)) short_matches.push_back(&model);
        if (short_matches.size() == 1) return short_matches[0]->path;

        // Prefix match: pick by preference order, else the first found.
        std::vector<const Discovered*> prefixed;
        for (const auto& model : found)
            if (model.alias.rfind(wanted, 0) == 0) prefixed.push_back(&model);
        if (!prefixed.empty()) {
            std::set<std::string> prefixed_aliases;
            for (const auto* model : prefixed) prefixed_aliases.insert(model->alias);
            for (const char* candidate : kPreference)
                if (prefixed_aliases.count(alias_from_filename(candidate)) != 0)
                    for (const auto* model : prefixed)
                        if (model->alias == alias_from_filename(candidate)) return model->path;
            return prefixed[0]->path;
        }
        return {};
    }
#if defined(__APPLE__)
    (void)gpu_ready;
    const auto& order = kPreference;
#else
    const auto& order = gpu_ready ? kPreference : kUntieredPreference;
#endif
    for (const char* name : order)
        for (const auto& dir : dirs) {
            std::filesystem::path candidate = dir / name;
            if (is_file(candidate)) return candidate;
        }
    return {};
}

std::filesystem::path resolve_vad(const std::vector<std::filesystem::path>& dirs) {
    for (const char* name : {"ggml-silero-v5.1.2.bin", "ggml-silero-v6.2.0.bin"})
        for (const auto& dir : dirs) {
            std::filesystem::path candidate = dir / name;
            if (is_file(candidate)) return candidate;
        }
    return {};
}

} // namespace mynah::models