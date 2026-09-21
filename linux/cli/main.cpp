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

namespace {

const char* kVersion = MYNAH_VERSION;
volatile std::sig_atomic_t g_signal = 0;

const char* state_word(mynah_state state) {
    switch (state) {
    case MYNAH_IDLE: return "idle";
    case MYNAH_LOADING: return "loading";
    case MYNAH_LISTENING: return "listening";
    case MYNAH_TRANSCRIBING: return "transcribing";
    }
    return "idle";
}

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
    const mynah::models::ModelInfo* info = mynah::models::find(name);
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
    // Candidates that are actually here; the benchmark is honest about a
    // model that did not load.
    std::vector<const mynah::models::ModelInfo*> candidates;
    for (const mynah::models::ModelInfo* info : {&mynah::models::kTurbo,
                                                 &mynah::models::kSmall,
                                                 &mynah::models::kBase}) {
        if (std::filesystem::exists(model_path(info))) candidates.push_back(info);
    }
    if (candidates.empty())
        return fail("no tier model on disk — download one first: mynah models download small");

    bool gpu_ready = false;
    std::optional<double> turbo_seconds;
    std::optional<double> small_seconds;
    auto stt = mynah::stt::make_whisper();
    for (const mynah::models::ModelInfo* info : candidates) {
        std::fprintf(stderr, "benchmarking %s … ", std::string(info->filename).c_str());
        auto result = mynah::models::benchmark(*stt, model_path(info), clip, language);
        if (!result) {
            std::fprintf(stderr, "could not run\n");
            continue;
        }
        std::fprintf(stderr, "%.2f s per clip\n", result->seconds);
        if (info->filename == mynah::models::kTurbo.filename) {
            turbo_seconds = result->seconds;
            gpu_ready = mynah::models::gpu_available(config.gpu);
        }
        if (info->filename == mynah::models::kSmall.filename)
            small_seconds = result->seconds;
    }
    stt->unload();

    std::string tier = mynah::models::choose_tier(gpu_ready, turbo_seconds, small_seconds);
    const mynah::models::ModelInfo* chosen = mynah::models::model_for_tier(tier);
    std::fprintf(stderr, "chosen tier: %s (%s)\n", tier.c_str(), std::string(chosen->filename).c_str());

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
// `model` always overrides; no candidates or no clip means no benchmark.
bool auto_benchmark(const mynah::config::Config& config) {
    if (!config.model.empty()) return false;
    if (!mynah::models::resolve(config.model, mynah::models::search_directories()).empty())
        return false; // something resolves already
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
    {"language", "Spoken language code"},
    {"model", "Speech model name or path (empty = the tier's default)"},
    {"prompt", "initial_prompt to bias recognition (empty = built-in)"},
    {"idle_timeout", "Seconds before the model unloads (0 = never)"},
    {"auto_stop_silence", "Seconds of silence that end a session (0 = off)"},
    {"vad", "Reject non-voice audio per utterance (Silero)"},
    {"gpu", "Run the gpu tier on a discrete GPU (off = integrated/CPU only)"},
    {"show_indicator", "The floating mic indicator (the desktop shell's pill)"},
    {"idle_visible", "Keep the indicator dimmed-visible between sessions"},
    {"injector", "smart, wtype or clipboard (empty = chosen for you)"},
    {"frame_energy", "Per-frame floor for speech (lower = more sensitive)"},
    {"min_energy", "Quietest utterance worth transcribing"},
    {"min_utterance", "Shortest utterance worth transcribing, in seconds"},
};

// Friendly names for `mynah set`, including the obvious aliases: people
// reasonably guess "lang", "key" or "sensitivity".
const std::pair<const char*, const char*> kFriendly[] = {
    {"hotkey", "hotkey"},           {"key", "hotkey"},
    {"trigger", "trigger"},         {"mode", "trigger"},
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
std::string show_number(double value) {
    return mynah::flat_toml::emit({{"x", mynah::flat_toml::real(value)}}).substr(4);
}

int config_command() {
    mynah::config::ReadResult read =
        mynah::config::read_file(mynah::config::default_path());
    if (read.status == mynah::config::ReadStatus::unreadable)
        std::fprintf(stderr, "mynah: %s\n", read.message.c_str());
    const mynah::config::Config& config = read.config;
    std::fprintf(stderr, "settings (%s)\n", mynah::config::default_path().c_str());
    std::fprintf(stderr, "  %-16s %-12s %s\n", "hotkey", config.hotkey.c_str(),
                 kSettings[0].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "trigger", config.trigger.c_str(),
                 kSettings[1].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "language", config.language.c_str(),
                 kSettings[2].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "model",
                 config.model.empty() ? "(default)" : config.model.c_str(),
                 kSettings[3].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "prompt",
                 config.prompt.empty() ? "(default)" : config.prompt.c_str(),
                 kSettings[4].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "idle_timeout",
                 show_number(config.idle_timeout).c_str(), kSettings[5].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "auto_stop_silence",
                 show_number(config.auto_stop_silence).c_str(), kSettings[6].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "vad", show_bool(config.vad).c_str(),
                 kSettings[7].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "gpu", show_bool(config.gpu).c_str(),
                 kSettings[8].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "show_indicator",
                 show_bool(config.show_indicator).c_str(), kSettings[9].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "idle_visible",
                 show_bool(config.idle_visible).c_str(), kSettings[10].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "injector",
                 config.injector.empty() ? "(smart)" : config.injector.c_str(),
                 kSettings[11].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "frame_energy",
                 show_number(config.frame_energy).c_str(), kSettings[12].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "min_energy",
                 show_number(config.min_energy).c_str(), kSettings[13].description);
    std::fprintf(stderr, "  %-16s %-12s %s\n", "min_utterance",
                 show_number(config.min_utterance).c_str(), kSettings[14].description);
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

std::string bands_json(const float* bands) {
    std::string json;
    for (int i = 0; i < mynah::constants::spectrum_bands; ++i) {
        char text[16];
        std::snprintf(text, sizeof(text), i ? ",%.3f" : "%.3f", double(bands[i]));
        json += text;
    }
    return json;
}

int run_engine() {
    mynah::config::Config config = mynah::config::load();
    if (auto_benchmark(config))
        config = mynah::config::load(); // the benchmark stored a choice

    // Typing first: the engine's TEXT event drives the injector, so it
    // must exist before the engine's callback fires.
    mynah::inject::Tools tools = mynah::inject::Tools::discover();
    std::unique_ptr<mynah::inject::Injector> injector;
    if (config.injector == "wtype") injector = mynah::inject::make_wtype(tools);
    else if (config.injector == "clipboard")
        injector = mynah::inject::make_clipboard(tools);
    else injector = mynah::inject::make_smart(tools);

    struct EngineContext {
        mynah_engine* engine = nullptr;
        mynah::control::Server* server = nullptr;
        mynah::inject::Injector* injector = nullptr;
    };
    EngineContext context;
    context.injector = injector.get();

    mynah_event_fn on_event = [](const mynah_event* event, void* user) {
        auto* ctx = static_cast<EngineContext*>(user);
        switch (mynah_event_get_kind(event)) {
        case MYNAH_EVENT_STATE:
            ctx->server->publish("{\"event\":\"state\",\"state\":\"" +
                                  std::string(state_word(mynah_event_state(event))) +
                                  "\"}");
            break;
        case MYNAH_EVENT_LEVEL:
            ctx->server->publish_level(double(mynah_event_level(event)),
                                       bands_json(mynah_event_bands(event)));
            break;
        case MYNAH_EVENT_TEXT: {
            // Publishing here means the event fires exactly when the text
            // really landed: a failed wtype publishes nothing, same as the
            // Python engine.
            const char* text = mynah_event_text(event);
            if (text != nullptr && ctx->injector->type_text(text))
                ctx->server->publish("{\"event\":\"text\",\"text\":" +
                                      mynah::json::quoted(text) + "}");
            break;
        }
        case MYNAH_EVENT_PROBLEM: {
            const char* code = mynah_event_problem_code(event);
            const char* message = mynah_event_problem_message(event);
            std::fprintf(stderr, "mynah: %s: %s\n", code ? code : "?",
                         message ? message : "");
            ctx->server->publish(
                "{\"event\":\"problem\",\"code\":" + mynah::json::quoted(code ? code : "") +
                ",\"message\":" + mynah::json::quoted(message ? message : "") + "}");
            break;
        }
        case MYNAH_EVENT_MODEL: {
            static const char* names[] = {"loading", "loaded", "unloaded"};
            std::string json = std::string("{\"event\":\"model\",\"status\":\"") +
                               names[int(mynah_event_model_status(event))] + "\"";
            if (const char* name = mynah_event_model_name(event))
                json += ",\"name\":" + mynah::json::quoted(name);
            json += "}";
            ctx->server->publish(json);
            break;
        }
        }
    };

    char* error = nullptr;
    mynah_engine* engine = mynah_create(nullptr, on_event, &context, &error);
    if (engine == nullptr) {
        fail(error ? error : "the engine could not start");
        std::free(error);
        return 1;
    }
    context.engine = engine;

    // The engine exists and nothing has started a session yet — no event
    // can fire — so the socket can be wired now.
    std::atomic<bool> quit_requested{false};
    mynah::control::Server::Handlers wired;
    wired.toggle = [engine] { mynah_toggle(engine); };
    wired.start = [engine] { mynah_start(engine); };
    wired.stop = [engine] { mynah_stop(engine); };
    wired.quit = [&quit_requested] { quit_requested.store(true); };
    wired.state = [engine] { return std::string(state_word(mynah_get_state(engine))); };
    mynah::control::Server server(std::move(wired), mynah::control::socket_path(), kVersion);
    try {
        server.start();
    } catch (const std::exception& e) {
        mynah_destroy(engine);
        return fail(e.what());
    }
    context.server = &server;

    // Capture: PipeWire straight into the engine's ring.
    mynah::capture::PipeWireCapture capture(engine);
    if (!capture.start()) {
        std::fprintf(stderr, "mynah: microphone: %s\n", capture.error().c_str());
        server.stop();
        mynah_destroy(engine);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::fprintf(stderr, "mynah %s listening on %s\n", kVersion, server.path().c_str());
    while (!quit_requested.load() && g_signal == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    capture.stop();
    mynah_stop(engine);
    server.stop();
    mynah_destroy(engine);
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
    if (command == "set" && args.size() > 1) return set_command(args[1]);
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
        for (std::size_t i = 1; i + 1 < args.size(); ++i)
            if (args[i] == "--timeout") timeout = std::stod(args[i + 1]);
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