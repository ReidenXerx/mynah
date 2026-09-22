// The C API. Everything below the lifecycle is the real engine since
// Phase 2: mynah_create builds a session::Engine around the config module,
// the pinned whisper.cpp, and Silero VAD; mynah_push_audio lands in the
// SPSC ring buffer; events arrive on the engine's threads exactly as the
// header contract says.

#include "mynah/mynah.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>

#include <whisper.h>

#include "config/config.hpp"
#include "config/flat_toml.hpp"
#include "json.hpp"
#include "models/resolve.hpp"
#include "session/session.hpp"
#include "stt/stt.hpp"
#include "vad/vad.hpp"

#ifndef MYNAH_VERSION
#define MYNAH_VERSION "0.0.0-dev" // set by CMake; this is a fallback only
#endif

// --- the event, defined here so no public layout leaks into the header ----

struct mynah_event {
    mynah_event_kind kind = MYNAH_EVENT_STATE;
    // MYNAH_EVENT_STATE
    mynah_state state = MYNAH_IDLE;
    // MYNAH_EVENT_LEVEL
    float level = 0.0f;
    std::array<float, MYNAH_SPECTRUM_BANDS> bands{};
    // MYNAH_EVENT_TEXT
    std::string text;
    // MYNAH_EVENT_PROBLEM
    std::string problem_code;
    std::string problem_message;
    // MYNAH_EVENT_MODEL
    mynah_model_status model_status = MYNAH_MODEL_UNLOADED;
    std::string model_name;
    std::string model_tier;
};

namespace {

mynah_state to_c(mynah::session::State state) {
    switch (state) {
    case mynah::session::State::Idle: return MYNAH_IDLE;
    case mynah::session::State::Loading: return MYNAH_LOADING;
    case mynah::session::State::Listening: return MYNAH_LISTENING;
    case mynah::session::State::Transcribing: return MYNAH_TRANSCRIBING;
    }
    return MYNAH_IDLE;
}

mynah_model_status to_c(mynah::session::ModelStatus status) {
    switch (status) {
    case mynah::session::ModelStatus::Loading: return MYNAH_MODEL_LOADING;
    case mynah::session::ModelStatus::Loaded: return MYNAH_MODEL_LOADED;
    case mynah::session::ModelStatus::Unloaded: return MYNAH_MODEL_UNLOADED;
    }
    return MYNAH_MODEL_UNLOADED;
}

char* error_string(const std::string& message) {
    // malloc'd: the contract in mynah.h says the caller frees().
    char* copy = static_cast<char*>(std::malloc(message.size() + 1));
    if (copy) std::memcpy(copy, message.c_str(), message.size() + 1);
    return copy;
}

} // namespace

struct mynah_engine {
    std::filesystem::path config_path;
    mynah_event_fn on_event = nullptr;
    void* user = nullptr;
    std::unique_ptr<mynah::session::Engine> session;

    void emit(const mynah_event& event) const {
        if (on_event) on_event(&event, user);
    }
};

extern "C" {

const char* mynah_version(void) {
    return MYNAH_VERSION;
}

mynah_engine* mynah_create(const char* config_path,
                           mynah_event_fn on_event,
                           void* user,
                           char** error) {
    if (error) *error = nullptr;
    try {
        auto engine = std::make_unique<mynah_engine>();
        mynah::config::Config config;
        if (config_path && *config_path) {
            // An explicit path is a front end pointing the core at a file it
            // prepared: read it, no whiz import, no write-back.
            std::filesystem::path path(config_path);
            mynah::config::ReadResult read = mynah::config::read_file(path);
            if (read.status == mynah::config::ReadStatus::unreadable) {
                if (error) *error = error_string(read.message);
                return nullptr;
            }
            config = read.config;
            engine->config_path = std::move(path);
        } else {
            // The default path is the core's own config: full semantics,
            // including the one-time whiz import.
            config = mynah::config::load();
            engine->config_path = mynah::config::default_path();
        }
        engine->on_event = on_event;
        engine->user = user;

        // The events, wrapped into mynah_event. The strings live in the
        // event struct, which lives on the stack of the emitting thread for
        // the duration of the callback.
        mynah::session::Events events;
        events.state = [engine_ptr = engine.get()](mynah::session::State state) {
            mynah_event event;
            event.kind = MYNAH_EVENT_STATE;
            event.state = to_c(state);
            engine_ptr->emit(event);
        };
        events.level = [engine_ptr = engine.get()](double level, const float* bands) {
            mynah_event event;
            event.kind = MYNAH_EVENT_LEVEL;
            event.level = static_cast<float>(level);
            for (int i = 0; i < MYNAH_SPECTRUM_BANDS; ++i) event.bands[i] = bands[i];
            engine_ptr->emit(event);
        };
        events.text = [engine_ptr = engine.get()](const std::string& text) {
            mynah_event event;
            event.kind = MYNAH_EVENT_TEXT;
            event.text = text;
            engine_ptr->emit(event);
        };
        events.problem =
            [engine_ptr = engine.get()](const char* code, const std::string& message) {
                mynah_event event;
                event.kind = MYNAH_EVENT_PROBLEM;
                event.problem_code = code;
                event.problem_message = message;
                engine_ptr->emit(event);
            };
        events.model = [engine_ptr = engine.get()](mynah::session::ModelStatus status,
                                                    const std::string& name) {
            mynah_event event;
            event.kind = MYNAH_EVENT_MODEL;
            event.model_status = to_c(status);
            event.model_name = name;
            engine_ptr->emit(event);
        };

        engine->session = std::make_unique<mynah::session::Engine>(
            std::move(config), mynah::stt::make_whisper(), mynah::vad::make_silero(),
            std::move(events));
        return engine.release();
    } catch (const std::exception& e) {
        if (error) *error = error_string(e.what());
        return nullptr;
    } catch (...) {
        if (error) *error = error_string("unknown error creating the engine");
        return nullptr;
    }
}

void mynah_destroy(mynah_engine* engine) {
    // Joins the engine's threads and frees the whisper and VAD contexts
    // before returning — ggml aborts at exit if a Metal context is still
    // alive.
    delete engine;
}

int mynah_toggle(mynah_engine* engine) {
    if (!engine) return -1;
    engine->session->toggle();
    return 0;
}

int mynah_start(mynah_engine* engine) {
    if (!engine) return -1;
    engine->session->start();
    return 0;
}

int mynah_stop(mynah_engine* engine) {
    if (!engine) return -1;
    engine->session->stop();
    return 0;
}

int mynah_ptt_press(mynah_engine* engine) {
    if (!engine) return -1;
    engine->session->ptt_press();
    return 0;
}

int mynah_ptt_release(mynah_engine* engine) {
    if (!engine) return -1;
    engine->session->ptt_release();
    return 0;
}

int mynah_push_audio(mynah_engine* engine, const float* samples, size_t count) {
    // The real-time call: capture thread only, never blocks, never
    // allocates — the ring buffer takes what fits and drops the rest.
    if (!engine) return -1;
    engine->session->push_audio(samples, count);
    return 0;
}

mynah_state mynah_get_state(const mynah_engine* engine) {
    if (!engine) return MYNAH_IDLE;
    return to_c(engine->session->state());
}

int mynah_reload_config(mynah_engine* engine) {
    if (!engine) return -1;
    try {
        mynah::config::Config fresh;
        if (engine->config_path == mynah::config::default_path()) {
            fresh = mynah::config::load();
        } else {
            mynah::config::ReadResult read = mynah::config::read_file(engine->config_path);
            if (read.status == mynah::config::ReadStatus::unreadable) return -1;
            fresh = read.config;
        }
        // Values a live session depends on take effect on the next session.
        engine->session->set_config(std::move(fresh));
        return 0;
    } catch (...) {
        return -1; // keep running on the config we already have
    }
}

// --- config (the settings surface for the front ends) ----------------------

// The config as a flat JSON object. Keys are snake_case, exactly the TOML
// keys, so the front end's adapter is a dictionary lookup with no mapping
// table to drift. C++ linkage: the extern "C" boundary is below, and this
// returns std::string.
// A config number as JSON. Two things the stream's default formatting got
// wrong: it printed six significant digits, so a hand-set frame_energy of
// 0.0123456789 came back to the settings UI as 0.0123457 and was written
// back rounded; and a non-finite value (a `nan` in the file, which TOML
// permits and JSON does not) produced "nan", which made the WHOLE object
// unparseable and every setting in the UI fall back to its default. null
// keeps the rest of the object valid, and a front end treats it as
// "absent" — the default for that one key. The shared formatter is the
// TOML writer's, so both spell numbers the same way, in any locale.
static std::string json_number(double value) {
    if (!std::isfinite(value)) return "null";
    return mynah::flat_toml::number_to_string(value);
}

static std::string config_to_json(const mynah::config::Config& config) {
    using mynah::json::quoted;
    std::ostringstream json;
    json << "{";
    json << "\"model\":" << quoted(config.model) << ',';
    json << "\"language\":" << quoted(config.language) << ',';
    json << "\"prompt\":" << quoted(config.prompt) << ',';
    json << "\"hotkey\":" << quoted(config.hotkey) << ',';
    json << "\"trigger\":" << quoted(config.trigger) << ',';
    json << "\"transcription_mode\":" << quoted(config.transcription_mode) << ',';
    json << "\"injector\":" << quoted(config.injector) << ',';
    json << "\"vad\":" << (config.vad ? "true" : "false") << ',';
    json << "\"gpu\":" << (config.gpu ? "true" : "false") << ',';
    json << "\"show_indicator\":" << (config.show_indicator ? "true" : "false") << ',';
    json << "\"idle_visible\":" << (config.idle_visible ? "true" : "false") << ',';
    json << "\"idle_timeout\":" << json_number(config.idle_timeout) << ',';
    json << "\"auto_stop_silence\":" << json_number(config.auto_stop_silence) << ',';
    json << "\"frame_energy\":" << json_number(config.frame_energy) << ',';
    json << "\"min_energy\":" << json_number(config.min_energy) << ',';
    json << "\"min_utterance\":" << json_number(config.min_utterance);
    json << "}"; // the closing brace: without it the JSON is truncated and
                 // every front end's parse falls back to defaults
    return json.str();
}

// Apply a JSON-encoded value to one config key. Returns false for an
// unknown key or a value of the wrong shape — the config file is never
// touched with a half-applied change.
bool apply_json_value(mynah::config::Config& config, const std::string& key,
                      const mynah::json::Value& value) {
    auto as_string = [&]() -> std::optional<std::string> {
        if (value.tag == mynah::json::Value::Tag::String) return value.text;
        return std::nullopt;
    };
    auto as_bool = [&]() -> std::optional<bool> {
        if (value.tag == mynah::json::Value::Tag::Bool) return value.boolean;
        return std::nullopt;
    };
    auto as_number = [&]() -> std::optional<double> {
        if (value.tag == mynah::json::Value::Tag::Number) return value.number;
        return std::nullopt;
    };

    if (key == "model") { if (auto v = as_string()) { config.model = *v; return true; } }
    else if (key == "language") { if (auto v = as_string()) { config.language = *v; return true; } }
    else if (key == "prompt") { if (auto v = as_string()) { config.prompt = *v; return true; } }
    else if (key == "hotkey") { if (auto v = as_string()) { config.hotkey = *v; return true; } }
    else if (key == "trigger") {
        if (auto v = as_string()) {
            if (*v == "toggle" || *v == "ptt") { config.trigger = *v; return true; }
        }
    } else if (key == "injector") {
        if (auto v = as_string()) {
            if (v->empty() || *v == "smart" || *v == "wtype" || *v == "clipboard") {
                config.injector = *v;
                return true;
            }
        }
    } else if (key == "transcription_mode") {
        if (auto v = as_string()) {
            if (*v == "live" || *v == "on_stop") { config.transcription_mode = *v; return true; }
        }
    } else if (key == "vad") { if (auto v = as_bool()) { config.vad = *v; return true; } }
    else if (key == "gpu") { if (auto v = as_bool()) { config.gpu = *v; return true; } }
    else if (key == "show_indicator") { if (auto v = as_bool()) { config.show_indicator = *v; return true; } }
    else if (key == "idle_visible") { if (auto v = as_bool()) { config.idle_visible = *v; return true; } }
    else if (key == "idle_timeout") { if (auto v = as_number()) { config.idle_timeout = *v; return true; } }
    else if (key == "auto_stop_silence") { if (auto v = as_number()) { config.auto_stop_silence = *v; return true; } }
    else if (key == "frame_energy") { if (auto v = as_number()) { config.frame_energy = *v; return true; } }
    else if (key == "min_energy") { if (auto v = as_number()) { config.min_energy = *v; return true; } }
    else if (key == "min_utterance") { if (auto v = as_number()) { config.min_utterance = *v; return true; } }
    return false;
}

char* mynah_config_json(const mynah_engine* engine) {
    if (!engine) return nullptr;
    try {
        std::string json = config_to_json(engine->session->config_snapshot());
        char* copy = static_cast<char*>(std::malloc(json.size() + 1));
        if (copy) std::memcpy(copy, json.c_str(), json.size() + 1);
        return copy;
    } catch (...) {
        return nullptr;
    }
}

int mynah_config_set(mynah_engine* engine, const char* key, const char* json_value) {
    if (!engine || !key || !json_value) return -1;
    try {
        mynah::json::Value value = mynah::json::parse(json_value);

        // Start from the FILE, not from the engine's copy of it. Saving
        // writes back every key this core owns, so starting from a snapshot
        // taken at launch wrote stale values over whatever another writer
        // had changed since — `mynah set sensitivity=…` in a terminal, or a
        // hand edit, was reverted by the next toggle in the settings window.
        // Re-reading first makes this what its name says: set ONE setting.
        mynah::config::ReadResult read = mynah::config::read_file(engine->config_path);
        if (read.status == mynah::config::ReadStatus::unreadable) return -1;
        mynah::config::Config config = read.config;

        if (!apply_json_value(config, key, value)) return -1;
        // To the engine's own file — not always the default one.
        mynah::config::save(config, engine->config_path);
        engine->session->set_config(std::move(config));
        return 0;
    } catch (...) {
        return -1;
    }
}

char* mynah_config_path(const mynah_engine* engine) {
    if (!engine) return nullptr;
    const std::string& path = engine->config_path.string();
    char* copy = static_cast<char*>(std::malloc(path.size() + 1));
    if (copy) std::memcpy(copy, path.c_str(), path.size() + 1);
    return copy;
}

char* mynah_find_model(const char* configured) {
    try {
        std::string wanted = configured ? configured : "";
#if defined(__APPLE__)
        const bool gpu_ready = true; // Metal (M4)
#else
        // The engine's own answer for an empty `model` (Session::load).
        const bool gpu_ready = mynah::stt::pick_gpu(mynah::config::load().gpu).has_value();
#endif
        std::filesystem::path found =
            mynah::models::resolve(wanted, mynah::models::search_directories(), gpu_ready);
        if (found.empty()) return nullptr;
        std::string path = found.string();
        char* copy = static_cast<char*>(std::malloc(path.size() + 1));
        if (copy) std::memcpy(copy, path.c_str(), path.size() + 1);
        return copy;
    } catch (...) {
        return nullptr;
    }
}

char* mynah_find_vad(void) {
    try {
        std::filesystem::path found =
            mynah::models::resolve_vad(mynah::models::search_directories());
        if (found.empty()) return nullptr;
        std::string path = found.string();
        char* copy = static_cast<char*>(std::malloc(path.size() + 1));
        if (copy) std::memcpy(copy, path.c_str(), path.size() + 1);
        return copy;
    } catch (...) {
        return nullptr;
    }
}

// --- the language table ------------------------------------------------------

int mynah_language_count(void) {
    return whisper_lang_max_id() + 1;
}

const char* mynah_language_code(int id) {
    return whisper_lang_str(id);
}

const char* mynah_language_name(int id) {
    return whisper_lang_str_full(id);
}

int mynah_language_id(const char* code) {
    if (!code) return -1;
    return whisper_lang_id(code);
}

mynah_event_kind mynah_event_get_kind(const mynah_event* event) {
    return event ? event->kind : MYNAH_EVENT_STATE;
}

mynah_state mynah_event_state(const mynah_event* event) {
    return event ? event->state : MYNAH_IDLE;
}

float mynah_event_level(const mynah_event* event) {
    return event ? event->level : 0.0f;
}

const float* mynah_event_bands(const mynah_event* event) {
    return event ? event->bands.data() : nullptr;
}

const char* mynah_event_text(const mynah_event* event) {
    return event ? event->text.c_str() : nullptr;
}

const char* mynah_event_problem_code(const mynah_event* event) {
    return event ? event->problem_code.c_str() : nullptr;
}

const char* mynah_event_problem_message(const mynah_event* event) {
    return event ? event->problem_message.c_str() : nullptr;
}

mynah_model_status mynah_event_model_status(const mynah_event* event) {
    return event ? event->model_status : MYNAH_MODEL_UNLOADED;
}

const char* mynah_event_model_name(const mynah_event* event) {
    return event && !event->model_name.empty() ? event->model_name.c_str() : nullptr;
}

const char* mynah_event_model_tier(const mynah_event* event) {
    // NULL on macOS (M4): no tiers there. Linux tiers arrive in Phase 3.
    return event && !event->model_tier.empty() ? event->model_tier.c_str() : nullptr;
}

} // extern "C"