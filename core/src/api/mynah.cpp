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
#include <string>

#include "config/config.hpp"
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