// Smoke tests for the Phase 1 C API: the stubs exist, hold the documented
// contract, and the config plumbing behind mynah_create is real. The
// session calls gain behaviour in Phase 2, at which point this suite grows
// into the real engine tests.

#include "vendor/doctest.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "env.hpp"
#include "mynah/mynah.h"

using mynah_test::EnvOverride;
using mynah_test::TmpDir;
using mynah_test::write_file;

TEST_CASE("mynah_version reports the project version") {
    CHECK(std::string(mynah_version()) == MYNAH_VERSION);
    CHECK(std::string(mynah_version()).size() >= 5); // at least "0.0.0"
}

TEST_CASE("an engine is created against an absent config and destroyed") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    char* error = nullptr;

    mynah_engine* engine = mynah_create(nullptr, nullptr, nullptr, &error);
    REQUIRE(engine != nullptr);
    CHECK(error == nullptr);
    CHECK(mynah_get_state(engine) == MYNAH_IDLE);
    mynah_destroy(engine);
}

TEST_CASE("an explicit config path works, absent file means defaults") {
    TmpDir dir;
    std::filesystem::path path = dir.path() / "custom.toml";
    char* error = nullptr;

    mynah_engine* engine = mynah_create(path.c_str(), nullptr, nullptr, &error);
    REQUIRE(engine != nullptr);
    CHECK(error == nullptr);
    CHECK(mynah_get_state(engine) == MYNAH_IDLE);
    mynah_destroy(engine);
}

TEST_CASE("an unreadable config fails create with an error string the caller frees") {
    TmpDir dir;
    std::filesystem::path path = dir.path() / "config.toml";
    std::filesystem::create_directories(path); // a directory: EISDIR, not "missing"
    char* error = nullptr;

    mynah_engine* engine = mynah_create(path.c_str(), nullptr, nullptr, &error);
    CHECK(engine == nullptr);
    REQUIRE(error != nullptr);
    CHECK(std::string(error).find("config.toml") != std::string::npos);
    std::free(error);
}

TEST_CASE("null handles are rejected, not crashed on") {
    float samples[16] = {};
    CHECK(mynah_toggle(nullptr) == -1);
    CHECK(mynah_start(nullptr) == -1);
    CHECK(mynah_stop(nullptr) == -1);
    CHECK(mynah_ptt_press(nullptr) == -1);
    CHECK(mynah_ptt_release(nullptr) == -1);
    CHECK(mynah_push_audio(nullptr, samples, 16) == -1);
    CHECK(mynah_reload_config(nullptr) == -1);
    CHECK(mynah_get_state(nullptr) == MYNAH_IDLE);
    mynah_destroy(nullptr); // no-op, must not crash
}

TEST_CASE("the C API drives a real session and reports a bad model through the callback") {
    // Since Phase 2 these calls run the engine rather than stubs, so this
    // case can no longer assert "state stays IDLE": start() is
    // asynchronous and the state moves on its own. It used to, and failed
    // about one run in three — and it loaded whatever model happened to be
    // in ~/.cache/whisper, which made a unit test depend on the machine.
    //
    // The config here names a model file that exists but is not a model,
    // so the load fails deterministically without touching the real cache:
    // the engine must report model_load_failed and settle back at IDLE.
    TmpDir dir;
    std::filesystem::path model = dir.path() / "ggml-not-a-model.bin";
    write_file(model, "certainly not ggml");
    std::filesystem::path config = dir.path() / "config.toml";
    write_file(config, "model = \"" + model.string() + "\"\nvad = false\nidle_timeout = 0.0\n");

    struct Sink {
        std::mutex mutex;
        std::vector<std::string> problems;
        std::atomic<int> states{0};
    } sink;

    mynah_engine* engine = mynah_create(
        config.c_str(),
        [](const mynah_event* event, void* user) {
            auto* out = static_cast<Sink*>(user);
            if (mynah_event_get_kind(event) == MYNAH_EVENT_PROBLEM) {
                std::lock_guard<std::mutex> lock(out->mutex);
                out->problems.push_back(mynah_event_problem_code(event));
            } else if (mynah_event_get_kind(event) == MYNAH_EVENT_STATE) {
                out->states.fetch_add(1);
            }
        },
        &sink, nullptr);
    REQUIRE(engine != nullptr);

    CHECK(mynah_start(engine) == 0);

    bool reported = false;
    for (int i = 0; i < 2000 && !reported; ++i) {
        {
            std::lock_guard<std::mutex> lock(sink.mutex);
            reported = !sink.problems.empty();
        }
        if (!reported) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    {
        std::lock_guard<std::mutex> lock(sink.mutex);
        REQUIRE(!sink.problems.empty());
        CHECK(sink.problems.front() == "model_load_failed");
    }
    CHECK(mynah_get_state(engine) == MYNAH_IDLE);

    // The rest of the surface stays callable on a session that never
    // started, and audio pushed at one is dropped rather than queued.
    float samples[16] = {};
    CHECK(mynah_push_audio(engine, samples, 16) == 0);
    CHECK(mynah_push_audio(engine, nullptr, 0) == 0);
    CHECK(mynah_stop(engine) == 0);
    CHECK(mynah_ptt_press(engine) == 0);
    CHECK(mynah_ptt_release(engine) == 0);
    CHECK(mynah_toggle(engine) == 0);
    mynah_stop(engine);

    mynah_destroy(engine);
}

TEST_CASE("reload_config picks up changes, and a broken file keeps the old config") {
    TmpDir dir;
    std::filesystem::path path = dir.path() / "config.toml";
    write_file(path, "language = \"ru\"\n");

    mynah_engine* engine = mynah_create(path.c_str(), nullptr, nullptr, nullptr);
    REQUIRE(engine != nullptr);

    write_file(path, "language = \"uk\"\n");
    CHECK(mynah_reload_config(engine) == 0);

    // Break the file: reload fails, the engine keeps running on what it has.
    std::filesystem::remove(path);
    std::filesystem::create_directories(path);
    CHECK(mynah_reload_config(engine) == -1);
    CHECK(mynah_get_state(engine) == MYNAH_IDLE);

    mynah_destroy(engine);
}