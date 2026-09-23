// mynah — the headless Linux binary (Phase 3).
//
// The command surface the Omarchy plugin drives, exactly the names it
// probes (docs/ENGINE-MIGRATION.md, Phase 3's command table): mynah,
// --version, toggle/start/stop/quit, status, watch, setup (doctor),
// config/set, service install/uninstall/status, models
// list/download/benchmark.
//
// The stderr wordings the plugin greps are load-bearing until its release
// that reads `problem` events: "not installed", "No whisper model" keep
// their Python-era spellings.

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "control_socket.hpp"
#include "downloads.hpp"
#include "injector.hpp"
#include "json.hpp"
#include "models/resolve.hpp"
#include "models/table.hpp"
#include "models/tiers.hpp"
#include "mynah/mynah.h"
#include "pipewire_capture.hpp"
#include "setup.hpp"
#include "stt/stt.hpp"
#include "systemd.hpp"
#include "tuning/constants.hpp"
#include "runtime.hpp"
#include "typing_queue.hpp"

namespace {

const char* kVersion = MYNAH_VERSION;
volatile std::sig_atomic_t g_signal = 0;


int fail(const std::string& message) {
    std::fprintf(stderr, "mynah: %s\n", message.c_str());
    return 1;
}

void on_signal(int) { g_signal = 1; }

// --- models ------------------------------------------------------------------------

// The bundled clip, packaged at /usr/share/mynah/benchmark.wav;
// MYNAH_BENCHMARK_CLIP points a dev checkout at any WAV.
std::string benchmark_clip() {
    if (const char* override_path = std::getenv("MYNAH_BENCHMARK_CLIP"))
        return override_path;
    for (const char* candidate : {"/usr/share/mynah/benchmark.wav",
                                  "/usr/local/share/mynah/benchmark.wav"})
        if (std::filesystem::exists(candidate)) return candidate;
    return "";
}

std::string model_path(const mynah::models::ModelInfo* info) {
    return (std::filesystem::path(mynah::models::download_dir()) /
            std::string(info->filename))
        .string();
}

int models_list() {
    std::fprintf(stderr, "models (%s)\n", mynah::models::download_dir().c_str());
    for (const mynah::models::ModelInfo* info : {&mynah::models::kTurbo,
                                                 &mynah::models::kSmall,
                                                 &mynah::models::kBase,
                                                 &mynah::models::kSileroVad}) {
        bool on_disk = std::filesystem::exists(model_path(info));
        std::fprintf(stderr, "  %-28s %-15s %s\n", std::string(info->filename).c_str(),
                     on_disk ? "on disk" : "not downloaded", std::string(info->url).c_str());
    }
    std::fprintf(stderr,
                 "\nDownload one with:  mynah models download <name>   e.g.  mynah models "
                 "download small\n");
    return 0;
}

int models_download(const std::string& name) {
    // The names this command advertises: the table's aliases and filenames
    // (find), plus "turbo" — a short alias find() deliberately leaves to
    // resolve() — the tier names, and "vad".
    const mynah::models::ModelInfo* info = mynah::models::find(name);
    if (info == nullptr && name == "turbo") info = &mynah::models::kTurbo;
    if (info == nullptr) info = mynah::models::model_for_tier(name); // gpu, small, base
    if (info == nullptr && name == "vad") info = &mynah::models::kSileroVad;
    if (info == nullptr)
        return fail("no model named '" + name +
                    "'. Known: turbo, small, base, vad — or a ggml filename.");
    std::fprintf(stderr, "downloading %s (%.1f GB)\n", std::string(info->filename).c_str(),
                 double(info->approximate_bytes) / 1e9);
    mynah::download::Result result = mynah::download::fetch(
        std::string(info->url), mynah::models::download_dir(), std::string(info->filename),
        std::string(info->sha256), [](std::uint64_t received, std::uint64_t total) {
            if (total == 0) return true;
            std::fprintf(stderr, "\r  %3d%%", int(received * 100 / total));
            return true;
        });
    if (!result.ok) return fail(result.error);
    std::fprintf(stderr, "\r  done: %s\n", result.path.c_str());
    return 0;
}

int models_benchmark(const std::string& clip, const std::string& language,
                     const mynah::config::Config& config) {
    // Turbo is only a candidate where it could win: the gpu tier needs a
    // GPU (choose_tier), and timing it on a CPU costs the first start
    // tens of seconds to learn nothing.
    std::optional<mynah::stt::GpuChoice> gpu = mynah::stt::pick_gpu(config.gpu);
    std::vector<const mynah::models::ModelInfo*> candidates;
    for (const mynah::models::ModelInfo* info : {&mynah::models::kTurbo,
                                                 &mynah::models::kSmall,
                                                 &mynah::models::kBase}) {
        if (!std::filesystem::exists(model_path(info))) continue;
        if (info == &mynah::models::kTurbo && !gpu) {
            std::fprintf(stderr, "skipping %s: no GPU to run it on%s\n",
                         std::string(info->filename).c_str(),
                         config.gpu ? "" : " (a discrete one needs: mynah set gpu=on)");
            continue;
        }
        candidates.push_back(info);
    }
    if (candidates.empty())
        return fail("no tier model on disk — download one first: mynah models download small");
    if (gpu) std::fprintf(stderr, "GPU: %s\n", gpu->name.c_str());

    // The benchmark is honest about a model that did not load.
    std::optional<double> turbo_seconds;
    std::optional<double> small_seconds;
    auto stt = mynah::stt::make_whisper();
    for (const mynah::models::ModelInfo* info : candidates) {
        std::fprintf(stderr, "benchmarking %s … ", std::string(info->filename).c_str());
        auto result =
            mynah::models::benchmark(*stt, model_path(info), clip, language, config.gpu);
        stt->unload(); // each candidate loads its own model
        if (!result) {
            std::fprintf(stderr, "could not run\n");
            continue;
        }
        std::fprintf(stderr, "%.2f s per clip\n", result->seconds);
        if (info == &mynah::models::kTurbo) turbo_seconds = result->seconds;
        if (info == &mynah::models::kSmall) small_seconds = result->seconds;
    }

    std::string tier = mynah::models::choose_tier(gpu.has_value(), turbo_seconds, small_seconds);
    const mynah::models::ModelInfo* chosen = mynah::models::model_for_tier(tier);
    std::fprintf(stderr, "chosen tier: %s (%s)\n", tier.c_str(),
                 std::string(chosen->filename).c_str());
    // The tier can be one whose model is not here — `base`, the floor, when
    // only a too-slow model was measured. Storing a path to nothing would
    // turn "slow" into "no model"; say what to fetch instead.
    if (!std::filesystem::exists(model_path(chosen)))
        return fail("the " + tier + " tier's model is not downloaded: mynah models download " +
                    tier + ", then mynah models benchmark");

    // Store the result: a set `model` always overrides, so writing it here
    // is what "stores the result" means — the next start skips the
    // benchmark and uses the chosen file directly.
    mynah::config::Config updated = config;
    updated.model = model_path(chosen);
    mynah::config::save(updated);
    return 0;
}

// The first start with no model configured: benchmark what is on disk and
// pick a tier (docs/ENGINE-MIGRATION.md, "Linux model tiers"). A set
// `model` always overrides; no clip or no candidate means no benchmark, and
// the engine falls back to kUntieredPreference.
bool auto_benchmark(const mynah::config::Config& config) {
    if (!config.model.empty()) return false;
    std::string clip = benchmark_clip();
    if (clip.empty()) return false;
    bool any_candidate = false;
    for (const mynah::models::ModelInfo* info : {&mynah::models::kTurbo,
                                                 &mynah::models::kSmall,
                                                 &mynah::models::kBase})
        if (std::filesystem::exists(model_path(info))) any_candidate = true;
    if (!any_candidate) return false;
    std::fprintf(stderr, "mynah: no model configured; benchmarking what is on disk\n");
    return models_benchmark(clip, config.language, config) == 0;
}

// --- config / set -------------------------------------------------------------------

struct SettingInfo {
    const char* key;
    const char* description;
};

const SettingInfo kSettings[] = {
    {"hotkey", "Not used here — your compositor binds a key to: mynah toggle"},
    {"trigger", "toggle (press to start and stop) or ptt (hold to talk)"},
    {"transcription_mode", "live (transcribe as you pause) or on_stop (transcribe the whole session at the end)"},
    {"language", "Spoken language code"},
    {"model", "Speech model name or path (empty = the tier's default)"},
    {"prompt", "initial_prompt to bias recognition (empty = built-in)"},
    {"idle_timeout", "Seconds before the model unloads (0 = never)"},
    {"auto_stop_silence", "Seconds of silence that end a session (0 = off)"},
    {"vad", "Reject non-voice audio per utterance (Silero)"},
    {"gpu", "Prefer a discrete GPU; it sleeps between sentences (off = integrated/CPU only)"},
    {"show_indicator", "The floating mic indicator (the desktop shell's pill)"},
    {"idle_visible", "Keep the indicator dimmed-visible between sessions"},
    {"injector", "smart (KWin: paste; elsewhere: wtype), wtype or clipboard"},
    {"frame_energy", "Per-frame floor for speech (lower = more sensitive)"},
    {"min_energy", "Quietest utterance worth transcribing"},
    {"min_utterance", "Shortest utterance worth transcribing, in seconds"},
};

// Friendly names for `mynah set`, including the obvious aliases: people
// reasonably guess "lang", "key" or "sensitivity".
const std::pair<const char*, const char*> kFriendly[] = {
    {"hotkey", "hotkey"},           {"key", "hotkey"},
    {"trigger", "trigger"},         {"mode", "trigger"},
    {"transcription_mode", "transcription_mode"}, {"batch", "transcription_mode"},
    {"language", "language"},       {"lang", "language"},
    {"model", "model"},             {"prompt", "prompt"},
    {"idle_timeout", "idle_timeout"}, {"idle", "idle_timeout"},
    {"timeout", "idle_timeout"},    {"vad", "vad"},
    {"gpu", "gpu"},                 {"auto_stop_silence", "auto_stop_silence"},
    {"silence", "auto_stop_silence"}, {"show_indicator", "show_indicator"},
    {"indicator", "show_indicator"}, {"idle_visible", "idle_visible"},
    {"idle_badge", "idle_visible"}, {"frame_energy", "frame_energy"},
    {"sensitivity", "frame_energy"}, {"min_energy", "min_energy"},
    {"min_utterance", "min_utterance"}, {"injector", "injector"},
};

std::string show_bool(bool value) { return value ? "on" : "off"; }
// As config.toml spells it.
std::string show_number(double value) { return mynah::flat_toml::number_to_string(value); }

int config_command() {
    mynah::config::ReadResult read =
        mynah::config::read_file(mynah::config::default_path());
    if (read.status == mynah::config::ReadStatus::unreadable)
        std::fprintf(stderr, "mynah: %s\n", read.message.c_str());
    const mynah::config::Config& config = read.config;
    std::fprintf(stderr, "settings (%s)\n", mynah::config::default_path().c_str());
    // Key, then the value as the engine sees it. One row per setting, driven
    // off the same table `set` validates against — the listing cannot drift
    // when a setting is added.
    // Columns as wide as the longest key ("transcription_mode").
    int key_width = 0;
    for (const SettingInfo& setting : kSettings)
        key_width = std::max(key_width, int(std::strlen(setting.key)));
    auto row = [&](const char* key, const std::string& value) {
        for (const SettingInfo& setting : kSettings)
            if (std::string(setting.key) == key) {
                std::fprintf(stderr, "  %-*s  %-12s %s\n", key_width, key, value.c_str(),
                             setting.description);
                return;
            }
    };
    row("hotkey", config.hotkey);
    row("trigger", config.trigger);
    row("transcription_mode", config.transcription_mode);
    row("language", config.language);
    row("model", config.model.empty() ? "(default)" : config.model);
    row("prompt", config.prompt.empty() ? "(default)" : config.prompt);
    row("idle_timeout", show_number(config.idle_timeout));
    row("auto_stop_silence", show_number(config.auto_stop_silence));
    row("vad", show_bool(config.vad));
    row("gpu", show_bool(config.gpu));
    row("show_indicator", show_bool(config.show_indicator));
    row("idle_visible", show_bool(config.idle_visible));
    row("injector", config.injector.empty() ? "(smart)" : config.injector);
    row("frame_energy", show_number(config.frame_energy));
    row("min_energy", show_number(config.min_energy));
    row("min_utterance", show_number(config.min_utterance));
    std::fprintf(stderr,
                 "\nChange one with:  mynah set <key>=<value>     e.g.  mynah set "
                 "language=uk\n");
    return read.status == mynah::config::ReadStatus::unreadable ? 1 : 0;
}

int set_command(const std::string& assignment) {
    std::size_t cut = assignment.find('=');
    if (cut == std::string::npos)
        return fail("Expected KEY=VALUE, e.g. mynah set hotkey=<f8>");
    std::string name = assignment.substr(0, cut);
    std::string raw = assignment.substr(cut + 1);
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));

    const char* key = nullptr;
    for (const auto& [alias, canonical] : kFriendly)
        if (name == alias) key = canonical;
    if (key == nullptr) return fail("Unknown setting '" + name + "'");

    // Coerce by the key's type, like the Python CLI did.
    mynah::flat_toml::Value value;
    std::string key_name = key;
    if (key_name == "trigger") {
        if (raw != "toggle" && raw != "ptt")
            return fail("trigger is one of toggle or ptt, got '" + raw + "'");
        value = mynah::flat_toml::str(raw);
    } else if (key_name == "transcription_mode") {
        if (raw != "live" && raw != "on_stop")
            return fail("transcription_mode is live or on_stop, got '" + raw + "'");
        value = mynah::flat_toml::str(raw);
    } else if (key_name == "vad" || key_name == "gpu" || key_name == "show_indicator" ||
               key_name == "idle_visible") {
        std::string lowered = raw;
        for (char& c : lowered) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (lowered == "on" || lowered == "true" || lowered == "yes" || lowered == "1")
            value = mynah::flat_toml::boolean(true);
        else if (lowered == "off" || lowered == "false" || lowered == "no" ||
                 lowered == "0")
            value = mynah::flat_toml::boolean(false);
        else
            return fail("Expected on or off, got '" + raw + "'");
    } else if (key_name == "idle_timeout" || key_name == "auto_stop_silence" ||
               key_name == "frame_energy" || key_name == "min_energy" ||
               key_name == "min_utterance") {
        try {
            value = mynah::flat_toml::real(std::stod(raw));
        } catch (const std::exception&) {
            return fail("Expected a number, got '" + raw + "'");
        }
    } else if (key_name == "injector") {
        if (!raw.empty() && raw != "smart" && raw != "wtype" && raw != "clipboard")
            return fail("injector is smart, wtype, clipboard or empty — got '" + raw + "'");
        value = mynah::flat_toml::str(raw);
    } else {
        value = mynah::flat_toml::str(raw);
    }

    // Read-modify-write onto the raw table so every writer's keys survive,
    // then through the typed loader so an invalid value cannot be written.
    mynah::flat_toml::Table current;
    std::error_code ec;
    if (std::filesystem::is_regular_file(mynah::config::default_path(), ec)) {
        std::ifstream in(mynah::config::default_path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        current = mynah::flat_toml::parse(text);
    }
    current[key_name] = value;
    mynah::config::save(mynah::config::from_values(current));
    std::fprintf(stderr, "mynah: %s = %s\n", key_name.c_str(), raw.c_str());
    std::fprintf(stderr, "saved to %s\n", mynah::config::default_path().c_str());
    return 0;
}

// --- control commands ----------------------------------------------------------------

int control_command(const std::string& cmd) {
    std::string reply;
    try {
        reply = mynah::control::command(cmd);
    } catch (const mynah::control::Error& e) {
        return fail(e.what());
    }
    mynah::json::Value parsed = mynah::json::parse(reply);
    const mynah::json::Value* ok = parsed.find("ok");
    if (ok == nullptr || ok->tag != mynah::json::Value::Tag::Bool || !ok->boolean) {
        const mynah::json::Value* error = parsed.find("error");
        return fail(error != nullptr && error->tag == mynah::json::Value::Tag::String
                        ? error->text
                        : "refused");
    }
    // Only `status` reports a state; the others are requests whose
    // outcome arrives as events.
    if (cmd == "status") {
        const mynah::json::Value* state = parsed.find("state");
        if (state != nullptr && state->tag == mynah::json::Value::Tag::String)
            std::printf("%s\n", state->text.c_str());
    }
    return 0;
}

int watch_command(double timeout_seconds) {
    int fd = -1;
    try {
        fd = mynah::control::connect(mynah::control::socket_path(), timeout_seconds);
    } catch (const mynah::control::Error& e) {
        return fail(e.what());
    }
    // The timeout was for CONNECTING. connect() leaves it on the socket as
    // SO_RCVTIMEO, and a stream that is quiet — an idle engine, a model
    // loading — is not a stream that ended: with it left in place `mynah
    // watch` exited after two silent seconds, and the Omarchy plugin read
    // each exit as "the engine is off" and dimmed the bird. Python's
    // watch cleared it the same way (conn.settimeout(None)).
    struct timeval no_timeout {};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));

    std::string request = "{\"cmd\": \"subscribe\"}\n";
    if (::write(fd, request.data(), request.size()) < 0) {
        ::close(fd);
        return fail("lost the connection to mynah");
    }
    // Every line goes to stdout — the subscribe acknowledgement included,
    // exactly as the Python `mynah watch` printed it. The Omarchy plugin
    // depends on that first line: its Service.qml reads
    // {"ok": true, "state": ...} as "a fresh connection — clear whatever
    // problem was showing, and adopt this state". Dropping it left a stale
    // error in the plugin's menu after every reconnect. After it, one JSON
    // line per event, flushed per line, until mynah quits or the reader
    // goes away.
    std::string buffer;
    char chunk[4096];
    ssize_t n;
    auto emit_lines = [&] {
        std::size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (line.empty()) continue;
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
    };
    while ((n = ::read(fd, chunk, sizeof(chunk))) > 0) {
        buffer.append(chunk, std::size_t(n));
        emit_lines();
    }
    ::close(fd);
    return 0;
}

// --- run: the engine ------------------------------------------------------------------


int run_engine() {
    mynah::config::Config config = mynah::config::load();
    if (auto_benchmark(config))
        config = mynah::config::load(); // the benchmark stored a choice

    // Everything else is the shared runtime (linux/common/runtime.hpp); the
    // terminal is its listener.
    std::atomic<bool> quit_requested{false};
    mynah::runtime::Listener listener;
    listener.problem = [](const std::string& code, const std::string& message) {
        std::fprintf(stderr, "mynah: %s: %s\n", code.empty() ? "?" : code.c_str(),
                     message.c_str());
    };
    listener.quit = [&quit_requested] { quit_requested.store(true); };
    mynah::runtime::Runtime runtime(std::move(listener));
    std::string error;
    if (!runtime.start(error)) return fail(error);
    std::fprintf(stderr, "mynah: speech runs on %s\n", runtime.speech_device().c_str());

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::fprintf(stderr, "mynah %s listening on %s\n", kVersion, runtime.socket_path().c_str());
    while (!quit_requested.load() && g_signal == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    runtime.stop();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return run_engine();

    const std::string& command = args[0];
    if (command == "--version" || command == "-v") {
        std::printf("mynah %s\n", kVersion);
        return 0;
    }
    if (command == "--help" || command == "-h") {
        std::printf(
            "mynah %s — say it, and it types where you are.\n\n"
            "  mynah                  start dictating: the compositor's key drives it\n"
            "  mynah setup            first run: typing, speech, microphone, hotkey\n"
            "  mynah config           what it is set to\n"
            "  mynah set KEY=VALUE    change one setting\n"
            "  mynah service …        install | uninstall | status of the login service\n"
            "  mynah models …         list | download <name> | benchmark\n\n"
            "  mynah toggle           start or end a session (bind your key to this)\n"
            "  mynah start/stop       drive a running mynah\n"
            "  mynah status           what it is doing right now\n"
            "  mynah watch            stream state, level and typed text as JSON lines\n"
            "  mynah quit             ask a running mynah to exit\n",
            kVersion);
        return 0;
    }
    if (command == "setup" || command == "doctor")
        return mynah::setup::report(mynah::setup::run_checks());
    if (command == "config" || command == "cfg") return config_command();
    if (command == "set") {
        if (args.size() != 2)
            return fail("set takes one KEY=VALUE, e.g. mynah set language=uk — "
                        "mynah config lists the keys");
        return set_command(args[1]);
    }
    if (command == "service") {
        std::string action = args.size() > 1 ? args[1] : "status";
        if (action == "install") return mynah::systemd::install();
        if (action == "uninstall" || action == "remove") return mynah::systemd::uninstall();
        if (action == "status" || action == "st") return mynah::systemd::status();
        return fail("service action is install, uninstall or status");
    }
    if (command == "toggle" || command == "start" || command == "stop" ||
        command == "quit")
        return control_command(command);
    if (command == "status") return control_command("status");
    if (command == "watch") {
        double timeout = 2.0;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] != "--timeout") continue;
            if (i + 1 == args.size()) return fail("--timeout needs a number of seconds");
            std::size_t used = 0;
            try {
                timeout = std::stod(args[i + 1], &used);
            } catch (const std::exception&) {
                used = 0;
            }
            if (used != args[i + 1].size() || !(timeout > 0.0))
                return fail("--timeout is a number of seconds above 0, got '" + args[i + 1] + "'");
            ++i;
        }
        return watch_command(timeout);
    }
    if (command == "models") {
        std::string action = args.size() > 1 ? args[1] : "list";
        if (action == "list") return models_list();
        if (action == "download" && args.size() > 2) return models_download(args[2]);
        if (action == "benchmark") {
            std::string clip = benchmark_clip();
            if (clip.empty())
                return fail("the benchmark clip is missing — the package should ship one at "
                            "/usr/share/mynah/benchmark.wav");
            mynah::config::Config config = mynah::config::load();
            return models_benchmark(clip, config.language, config);
        }
        return fail("models action is list, download <name> or benchmark");
    }
    return fail("unknown command '" + command + "' — try: mynah --help");
}