// mynah-replay — a WAV through the full engine, events and transcripts out.
//
// The Phase 2 dev tool (docs/ENGINE-MIGRATION.md): a 16 kHz mono s16le WAV
// in, the real engine driven end to end (config → detector → gates → merge
// → VAD → whisper.cpp → filter → spacing), one JSON line per event out.
// The audio is pushed faster than real time — every gate in the engine is
// frame-driven, not wall-clock-driven, so segmentation is unaffected.
//
// Two waits are load-bearing, and without them the tool reported success
// having dictated nothing:
//   - mynah_start is ASYNCHRONOUS. Audio pushed before the session reaches
//     LISTENING is dropped on the floor (the ring only accepts while
//     capturing), and a stop that arrives during the load cancels the start
//     outright. So wait for LISTENING before the first push.
//   - The audio worker stops popping the ring the moment stop() is called,
//     so stopping straight after the last push throws away whatever is
//     still queued. Level events are emitted one per processed frame, so
//     the ring is drained once they go quiet.
//
//   mynah-replay speech.wav
//   MYNAH_CONFIG_DIR=/tmp/profile mynah-replay speech.wav --levels
//   mynah-replay speech.wav --config /tmp/profile/config.toml
//
// Exit 0 with the event log; 1 on usage/engine failure. Problem events
// (no_model, model_load_failed) appear as JSON lines like every other
// event and fail the run.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/wav.hpp"
#include "mynah/mynah.h"

namespace {

std::string json_escape(const char* text) {
    if (!text) return "";
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        switch (*p) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (*p < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
                out += buf;
            } else {
                out += char(*p);
            }
        }
    }
    return out;
}

const char* state_name(mynah_state state) {
    switch (state) {
    case MYNAH_IDLE: return "idle";
    case MYNAH_LOADING: return "loading";
    case MYNAH_LISTENING: return "listening";
    case MYNAH_TRANSCRIBING: return "transcribing";
    }
    return "?";
}

const char* model_status_name(mynah_model_status status) {
    switch (status) {
    case MYNAH_MODEL_LOADING: return "loading";
    case MYNAH_MODEL_LOADED: return "loaded";
    case MYNAH_MODEL_UNLOADED: return "unloaded";
    }
    return "?";
}

// The event log. Events arrive on the engine's threads; the vector is
// guarded and appended in arrival order.
struct EventLog {
    std::mutex mutex;
    std::vector<std::string> lines;
    bool want_levels = false; // --levels: include the 33/s level events
    // Every processed frame emits one level event: the clock that says the
    // ring has been drained. Counted even when the levels are not printed.
    std::atomic<std::uint64_t> levels{0};
    std::atomic<int> texts{0};
    std::atomic<int> problems{0};

    void add(std::string line) {
        std::lock_guard<std::mutex> lock(mutex);
        lines.push_back(std::move(line));
    }
};

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path wav_path;
    const char* config_path = nullptr;
    bool levels = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--levels") levels = true;
        else if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (wav_path.empty()) wav_path = arg;
        else {
            std::fprintf(stderr, "usage: mynah-replay <audio.wav> [--config path] [--levels]\n");
            return 1;
        }
    }
    if (wav_path.empty()) {
        std::fprintf(stderr, "usage: mynah-replay <audio.wav> [--config path] [--levels]\n");
        return 1;
    }

    std::vector<float> samples;
    try {
        samples = mynah::audio::read_wav_mono_16k(wav_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mynah-replay: %s\n", e.what());
        return 1;
    }

    EventLog log;
    mynah_event_fn on_event = [](const mynah_event* event, void* user) {
        auto* sink = static_cast<EventLog*>(user);
        switch (mynah_event_get_kind(event)) {
        case MYNAH_EVENT_STATE:
            sink->add(std::string("{\"event\":\"state\",\"state\":\"") +
                      state_name(mynah_event_state(event)) + "\"}");
            break;
        case MYNAH_EVENT_LEVEL: {
            sink->levels.fetch_add(1, std::memory_order_relaxed);
            if (!sink->want_levels) break;
            std::string line = "{\"event\":\"level\",\"level\":";
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.3f", double(mynah_event_level(event)));
            line += buf;
            line += ",\"bands\":[";
            const float* bands = mynah_event_bands(event);
            for (int i = 0; i < MYNAH_SPECTRUM_BANDS; ++i) {
                std::snprintf(buf, sizeof(buf), i ? ",%.3f" : "%.3f", double(bands[i]));
                line += buf;
            }
            line += "]}";
            sink->add(std::move(line));
            break;
        }
        case MYNAH_EVENT_TEXT:
            sink->texts.fetch_add(1, std::memory_order_relaxed);
            sink->add("{\"event\":\"text\",\"text\":\"" +
                      std::string(json_escape(mynah_event_text(event))) + "\"}");
            break;
        case MYNAH_EVENT_PROBLEM:
            sink->problems.fetch_add(1, std::memory_order_relaxed);
            sink->add(std::string("{\"event\":\"problem\",\"code\":\"") +
                      json_escape(mynah_event_problem_code(event)) + "\",\"message\":\"" +
                      json_escape(mynah_event_problem_message(event)) + "\"}");
            break;
        case MYNAH_EVENT_MODEL: {
            std::string line = std::string("{\"event\":\"model\",\"status\":\"") +
                               model_status_name(mynah_event_model_status(event)) + "\"";
            if (const char* name = mynah_event_model_name(event)) {
                line += ",\"name\":\"" + json_escape(name) + "\"";
            }
            line += "}";
            sink->add(std::move(line));
            break;
        }
        }
    };
    log.want_levels = levels;

    char* error = nullptr;
    mynah_engine* engine = mynah_create(config_path, on_event, &log, &error);
    if (!engine) {
        std::fprintf(stderr, "mynah-replay: %s\n", error ? error : "engine create failed");
        std::free(error);
        return 1;
    }

    mynah_start(engine);

    // Wait for the session to be live. A cold load compiles the Metal
    // library the first time ever (~7 s), so the budget is generous; a
    // model that never loads reports itself as a problem event and the
    // state goes back to idle, which ends the wait too.
    constexpr auto kStartBudget = std::chrono::seconds(120);
    auto deadline = std::chrono::steady_clock::now() + kStartBudget;
    while (mynah_get_state(engine) != MYNAH_LISTENING &&
           std::chrono::steady_clock::now() < deadline &&
           log.problems.load(std::memory_order_relaxed) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (mynah_get_state(engine) != MYNAH_LISTENING) {
        std::fprintf(stderr,
                     "mynah-replay: the session never started listening — "
                     "no audio was pushed\n");
        {
            std::lock_guard<std::mutex> lock(log.mutex);
            for (const std::string& line : log.lines) std::printf("%s\n", line.c_str());
        }
        mynah_destroy(engine);
        return 1;
    }

    mynah_start(engine);
    // 1 s chunks with a breather: far faster than real time, slow enough
    // that the worker drains every chunk (dropped audio would be a lie).
    constexpr std::size_t kChunk = 16000;
    for (std::size_t i = 0; i < samples.size(); i += kChunk) {
        std::size_t count = std::min(kChunk, samples.size() - i);
        mynah_push_audio(engine, samples.data() + i, count);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Let the ring drain: stop() ends the audio worker's loop at once, so
    // anything it has not popped yet would be thrown away. The level
    // events going quiet for a few frame-times means there is nothing left
    // to pop. Bounded, so a wedged worker cannot hang the tool.
    auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::uint64_t seen = log.levels.load(std::memory_order_relaxed);
    auto quiet_since = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < drain_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::uint64_t now_seen = log.levels.load(std::memory_order_relaxed);
        if (now_seen != seen) {
            seen = now_seen;
            quiet_since = std::chrono::steady_clock::now();
            continue;
        }
        if (std::chrono::steady_clock::now() - quiet_since > std::chrono::milliseconds(300))
            break;
    }

    mynah_stop(engine);
    // Blocks until every queued utterance is transcribed, then frees the
    // model — the log is complete and ordered when destroy returns.
    mynah_destroy(engine);

    std::lock_guard<std::mutex> lock(log.mutex);
    for (const std::string& line : log.lines) std::printf("%s\n", line.c_str());

    // The summary goes to stderr so stdout stays a clean event stream. A
    // run that transcribed nothing is not by itself a failure — silence and
    // rejected noise are correct answers — but it is the thing a person
    // reading this output wants to know first.
    std::fprintf(stderr,
                 "mynah-replay: %.2f s of audio, %llu frames processed, %d text event(s)\n",
                 samples.size() / 16000.0,
                 (unsigned long long)log.levels.load(std::memory_order_relaxed),
                 log.texts.load(std::memory_order_relaxed));
    return log.problems.load(std::memory_order_relaxed) > 0 ? 1 : 0;
}