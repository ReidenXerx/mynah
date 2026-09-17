// The C API, Phase 1 skeleton (docs/ENGINE-MIGRATION.md). The header is the
// contract front ends compile against; the engine behind it lands in
// Phase 2. What is real here today: the version, config loading (the core
// owns config.toml, P5), and the threading-safe state field the stubs expose.
// The session calls all succeed without doing anything yet — state stays
// MYNAH_IDLE — and mynah_push_audio accepts and drops samples until the
// SPSC ring buffer exists.

#include "mynah/mynah.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>

#include "config/config.hpp"

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

struct mynah_engine {
    // The file the engine was created with; reload_config re-reads it.
    std::filesystem::path config_path;
    mynah::config::Config config;
    mynah_event_fn on_event = nullptr;
    void* user = nullptr;
    mutable std::mutex mutex; // get_state locks on a const engine
    mynah_state state = MYNAH_IDLE;
};

namespace {

char* error_string(const std::string& message) {
    // malloc'd: the contract in mynah.h says the caller frees().
    char* copy = static_cast<char*>(std::malloc(message.size() + 1));
    if (copy) std::memcpy(copy, message.c_str(), message.size() + 1);
    return copy;
}

} // namespace

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
        if (config_path && *config_path) {
            // An explicit path is a front end pointing the core at a file it
            // prepared: read it, no whiz import, no write-back.
            std::filesystem::path path(config_path);
            mynah::config::ReadResult read = mynah::config::read_file(path);
            if (read.status == mynah::config::ReadStatus::unreadable) {
                if (error) *error = error_string(read.message);
                return nullptr;
            }
            engine->config = read.config;
            engine->config_path = std::move(path);
        } else {
            // The default path is the core's own config: full semantics,
            // including the one-time whiz import.
            engine->config = mynah::config::load();
            engine->config_path = mynah::config::default_path();
        }
        engine->on_event = on_event;
        engine->user = user;
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
    // Frees whisper and VAD contexts before returning once they exist
    // (Phase 2); ggml aborts at exit if a Metal context is still alive.
    delete engine;
}

int mynah_toggle(mynah_engine* engine) { return engine ? 0 : -1; }
int mynah_start(mynah_engine* engine) { return engine ? 0 : -1; }
int mynah_stop(mynah_engine* engine) { return engine ? 0 : -1; }
int mynah_ptt_press(mynah_engine* engine) { return engine ? 0 : -1; }
int mynah_ptt_release(mynah_engine* engine) { return engine ? 0 : -1; }

int mynah_push_audio(mynah_engine* engine, const float* samples, size_t count) {
    // Phase 1: samples are accepted and dropped. Phase 2 puts an SPSC ring
    // buffer here — this is the real-time call and must never block.
    (void)samples;
    (void)count;
    return engine ? 0 : -1;
}

mynah_state mynah_get_state(const mynah_engine* engine) {
    if (!engine) return MYNAH_IDLE;
    std::lock_guard<std::mutex> lock(engine->mutex);
    return engine->state;
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
        std::lock_guard<std::mutex> lock(engine->mutex);
        engine->config = std::move(fresh);
        return 0;
    } catch (...) {
        return -1; // keep running on the config we already have
    }
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
    // NULL on macOS (M4): no tiers there.
    return event && !event->model_tier.empty() ? event->model_tier.c_str() : nullptr;
}

} // extern "C"