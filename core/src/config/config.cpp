#include "config.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

namespace mynah::config {

namespace {

std::filesystem::path home() {
    const char* home_env = std::getenv("HOME");
    return home_env ? std::filesystem::path(home_env) : std::filesystem::path(".");
}

std::string read_text(const std::filesystem::path& path, bool& ok) {
    // A directory (or special file) sitting where config.toml should be is a
    // read failure, not a missing file — and ifstream's behaviour on a
    // directory differs by platform, so check before opening.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        ok = false;
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = static_cast<bool>(in);
    if (in.bad()) {
        ok = false;
        return {};
    }
    return buffer.str();
}

// The string for a key, or the default if absent/not a string.
std::string string_or(const flat_toml::Table& values, const char* key,
                      std::string fallback) {
    if (auto it = values.find(key); it != values.end())
        if (const auto* s = std::get_if<std::string>(&it->second)) return *s;
    return fallback;
}

// TOML has no float/int coercion, but Python writes `45.0` as a float and `0`
// as an int for the same field, so accept either (Swift's number()).
double number_or(const flat_toml::Table& values, const char* key, double fallback) {
    if (auto it = values.find(key); it != values.end()) {
        if (const auto* d = std::get_if<double>(&it->second)) return *d;
        if (const auto* i = std::get_if<std::int64_t>(&it->second))
            return static_cast<double>(*i);
    }
    return fallback;
}

bool bool_or(const flat_toml::Table& values, const char* key, bool fallback) {
    if (auto it = values.find(key); it != values.end())
        if (const auto* b = std::get_if<bool>(&it->second)) return *b;
    return fallback;
}

// Set a double key only when the value is a number (int counts); used by the
// legacy import, which must not import a mistyped value as a default.
bool set_number(const flat_toml::Table& values, const std::string& key, double& out) {
    if (auto it = values.find(key); it != values.end()) {
        if (const auto* d = std::get_if<double>(&it->second)) {
            out = *d;
            return true;
        }
        if (const auto* i = std::get_if<std::int64_t>(&it->second)) {
            out = static_cast<double>(*i);
            return true;
        }
    }
    return false;
}

bool set_string(const flat_toml::Table& values, const std::string& key, std::string& out) {
    if (auto it = values.find(key); it != values.end())
        if (const auto* s = std::get_if<std::string>(&it->second)) {
            out = *s;
            return true;
        }
    return false;
}

bool set_bool(const flat_toml::Table& values, const std::string& key, bool& out) {
    if (auto it = values.find(key); it != values.end())
        if (const auto* b = std::get_if<bool>(&it->second)) {
            out = *b;
            return true;
        }
    return false;
}

} // namespace

std::filesystem::path default_path() {
    const char* override_dir = std::getenv("MYNAH_CONFIG_DIR");
    if (override_dir && *override_dir)
        return std::filesystem::path(override_dir) / "config.toml";
    return home() / ".config" / "mynah" / "config.toml";
}

std::filesystem::path legacy_path() {
    const char* override_path = std::getenv("MYNAH_LEGACY_CONFIG");
    if (override_path && *override_path) return std::filesystem::path(override_path);
    return home() / ".config" / "whiz" / "config.toml";
}

ReadResult read_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) {
        ReadResult result;
        result.config = Config{};
        result.status = ReadStatus::missing;
        return result;
    }
    bool ok = false;
    std::string text = read_text(path, ok);
    if (!ok) {
        ReadResult result;
        result.status = ReadStatus::unreadable;
        result.message = "config.toml unreadable — using defaults: " + path.string();
        return result;
    }
    ReadResult result;
    result.config = from_values(flat_toml::parse(text));
    return result;
}

Config from_values(const flat_toml::Table& values) {
    Config c;
    c.model = string_or(values, "model", c.model);
    c.language = string_or(values, "language", c.language);
    c.prompt = string_or(values, "prompt", c.prompt);
    c.idle_timeout = number_or(values, "idle_timeout", c.idle_timeout);
    c.hotkey = string_or(values, "hotkey", c.hotkey);
    c.trigger = string_or(values, "trigger", c.trigger);
    c.vad = bool_or(values, "vad", c.vad);
    c.auto_stop_silence = number_or(values, "auto_stop_silence", c.auto_stop_silence);
    c.show_indicator = bool_or(values, "show_indicator", c.show_indicator);
    c.idle_visible = bool_or(values, "idle_visible", c.idle_visible);
    c.injector = string_or(values, "injector", c.injector);
    c.gpu = bool_or(values, "gpu", c.gpu);
    c.frame_energy = number_or(values, "frame_energy", c.frame_energy);
    c.min_energy = number_or(values, "min_energy", c.min_energy);
    c.min_utterance = number_or(values, "min_utterance", c.min_utterance);
    return c;
}

void merge_into(flat_toml::Table& values, const Config& config) {
    values["model"] = flat_toml::str(config.model);
    values["language"] = flat_toml::str(config.language);
    values["prompt"] = flat_toml::str(config.prompt);
    values["idle_timeout"] = flat_toml::real(config.idle_timeout);
    values["hotkey"] = flat_toml::str(config.hotkey);
    values["trigger"] = flat_toml::str(config.trigger);
    values["vad"] = flat_toml::boolean(config.vad);
    values["auto_stop_silence"] = flat_toml::real(config.auto_stop_silence);
    values["show_indicator"] = flat_toml::boolean(config.show_indicator);
    values["idle_visible"] = flat_toml::boolean(config.idle_visible);
    values["injector"] = flat_toml::str(config.injector);
    values["gpu"] = flat_toml::boolean(config.gpu);
    values["frame_energy"] = flat_toml::real(config.frame_energy);
    values["min_energy"] = flat_toml::real(config.min_energy);
    values["min_utterance"] = flat_toml::real(config.min_utterance);
}

namespace {

// The dictate_* keys from whiz's config, with the prefix stripped — the
// one-time import. Values of the wrong type are skipped rather than trusted:
// a config that says `dictate_vad = "yes"` imports nothing for vad.
std::vector<std::pair<std::string, flat_toml::Value>> legacy_values() {
    std::filesystem::path path = legacy_path();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return {};
    bool ok = false;
    std::string text = read_text(path, ok);
    if (!ok) return {};
    std::vector<std::pair<std::string, flat_toml::Value>> out;
    for (const auto& [key, value] : flat_toml::parse(text)) {
        if (key.rfind("dictate_", 0) != 0) continue;
        std::string name = key.substr(8);
        // Keep only keys this Config owns.
        if (name == "model" || name == "language" || name == "prompt" ||
            name == "hotkey" || name == "trigger" || name == "injector")
            if (std::holds_alternative<std::string>(value)) out.emplace_back(name, value);
        if (name == "idle_timeout" || name == "auto_stop_silence" ||
            name == "frame_energy" || name == "min_energy" || name == "min_utterance")
            if (std::holds_alternative<double>(value) ||
                std::holds_alternative<std::int64_t>(value))
                out.emplace_back(name, value);
        if (name == "vad" || name == "show_indicator" || name == "idle_visible")
            if (std::holds_alternative<bool>(value)) out.emplace_back(name, value);
    }
    return out;
}

} // namespace

Config load() {
    std::filesystem::path path = default_path();
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
        bool ok = false;
        std::string text = read_text(path, ok);
        if (!ok)
            throw Error("config.toml exists but could not be read: " + path.string() +
                        "\nFix or delete the file by hand (a deleted file regenerates "
                        "from defaults)");
        return from_values(flat_toml::parse(text));
    }

    // No config yet — first run. Offer the whiz settings, and keep them.
    auto imported = legacy_values();
    if (imported.empty()) return Config{};

    Config config;
    for (const auto& [name, value] : imported) {
        if (const auto* s = std::get_if<std::string>(&value)) {
            if (name == "model") config.model = *s;
            else if (name == "language") config.language = *s;
            else if (name == "prompt") config.prompt = *s;
            else if (name == "hotkey") config.hotkey = *s;
            else if (name == "trigger") config.trigger = *s;
            else if (name == "injector") config.injector = *s;
        } else if (const auto* b = std::get_if<bool>(&value)) {
            if (name == "vad") config.vad = *b;
            else if (name == "show_indicator") config.show_indicator = *b;
            else if (name == "idle_visible") config.idle_visible = *b;
        } else {
            double d = std::holds_alternative<std::int64_t>(value)
                           ? static_cast<double>(std::get<std::int64_t>(value))
                           : std::get<double>(value);
            if (name == "idle_timeout") config.idle_timeout = d;
            else if (name == "auto_stop_silence") config.auto_stop_silence = d;
            else if (name == "frame_energy") config.frame_energy = d;
            else if (name == "min_energy") config.min_energy = d;
            else if (name == "min_utterance") config.min_utterance = d;
        }
    }
    save(config);
    return config;
}

namespace {

// The file a save should replace: `path` itself, or — when it is a symlink —
// the file at the end of the chain. Renaming over the link would turn a
// dotfiles-managed config into a plain file and leave the real one unchanged;
// Python's write_text follows the link, and so does this. A dangling link
// resolves to where its target would be, so the save creates it there.
std::filesystem::path write_target(const std::filesystem::path& path) {
    std::filesystem::path target = path;
    std::error_code ec;
    for (int hops = 0; hops < 40 && std::filesystem::is_symlink(target, ec) && !ec; ++hops) {
        std::filesystem::path link = std::filesystem::read_symlink(target, ec);
        if (ec) break;
        target = link.is_absolute() ? link : target.parent_path() / link;
    }
    return target;
}

void write_all(int fd, const std::string& text) {
    const char* data = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        ssize_t written = ::write(fd, data, left);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw Error(std::string("write failed: ") + std::strerror(errno));
        }
        data += written;
        left -= static_cast<std::size_t>(written);
    }
}

} // namespace

void save(const Config& config) {
    std::filesystem::path path = default_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec); // first save makes ~/.config/mynah

    std::filesystem::path target = write_target(path);

    // Read-modify-write, not overwrite: the file is co-owned by writers this
    // core has never heard of. A file we cannot read at all is replaced —
    // there is nothing left to preserve.
    flat_toml::Table values;
    bool ok = false;
    std::string existing = read_text(target, ok);
    if (ok) values = flat_toml::parse(existing);

    merge_into(values, config);
    std::string text = flat_toml::emit(values);

    // Atomic replace: write a temp file beside the target, then rename over it.
    // The temp name is unique (mkstemp), so two writers saving at once — the
    // KDE app and `mynah set`, say — cannot write into each other's temp file.
    std::string temp = (target.parent_path() / ("." + target.filename().string() + ".XXXXXX")).string();
    int fd = ::mkstemp(temp.data());
    if (fd < 0)
        throw Error("cannot write " + target.string() + ": " + std::strerror(errno));

    try {
        // mkstemp creates 0600. Keep the mode the file already had; a new
        // config gets the usual 0644.
        struct stat existing_stat {};
        mode_t mode = ::stat(target.c_str(), &existing_stat) == 0
                          ? (existing_stat.st_mode & 07777)
                          : 0644;
        if (::fchmod(fd, mode) != 0)
            throw Error(std::string("cannot set permissions: ") + std::strerror(errno));
        write_all(fd, text);
        if (::close(fd) != 0) {
            fd = -1;
            throw Error(std::string("close failed: ") + std::strerror(errno));
        }
        fd = -1;
    } catch (const Error& e) {
        if (fd >= 0) ::close(fd);
        ::unlink(temp.c_str());
        throw Error("cannot write " + target.string() + ": " + e.what());
    }

    if (::rename(temp.c_str(), target.c_str()) != 0) {
        int error = errno;
        ::unlink(temp.c_str());
        throw Error("cannot replace " + target.string() + ": " + std::strerror(error));
    }
}

} // namespace mynah::config