// The session state machine, driven with fake STT and VAD — the twin of
// the lifecycle halves of tests/test_dictate.py and
// macos/Tests/MynahAppTests's SessionController semantics: toggle, PTT,
// cancel-a-start-while-loading, the enqueue gates, merge, the spacing
// rule, auto-stop, idle unload, and destroy-drains.
//
// The fakes let the tests assert without a model file; the real whisper
// path is exercised end to end by mynah-replay.

#include "vendor/doctest.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "env.hpp"
#include "fakes.hpp"
#include "models/resolve.hpp"
#include "session/session.hpp"
#include "stt/stt.hpp"
#include "vad/vad.hpp"

namespace {

using mynah::session::Engine;
using mynah::session::Events;
using mynah::session::ModelStatus;
using mynah::session::State;
using mynah_test::FakeStt;
using mynah_test::FakeVad;

// An event tape: everything the engine reports, in order.
struct Tape {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> lines;

    void add(const std::string& line) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            lines.push_back(line);
        }
        cv.notify_all();
    }
    bool contains(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& line : lines)
            if (line.find(needle) != std::string::npos) return true;
        return false;
    }
    int count(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex);
        int n = 0;
        for (const auto& line : lines)
            if (line.find(needle) != std::string::npos) ++n;
        return n;
    }
    std::vector<std::string> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return lines;
    }
    // Waits until the needle has been seen n times (or the timeout — the
    // caller's own assertion then fails with a better message than a hang).
    bool await_count(const std::string& needle, int n, int timeout_ms = 10000) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            int seen = 0;
            for (const auto& line : lines)
                if (line.find(needle) != std::string::npos) ++seen;
            return seen >= n;
        });
    }
    bool await(const std::string& needle, int timeout_ms = 10000) {
        return await_count(needle, 1, timeout_ms);
    }
};

// The harness: an engine with the fakes wired in and events on tape. The
// model resolves to a dummy file the harness creates, so no test depends
// on what models happen to be on the machine.
struct Harness {
    mynah_test::TmpDir dir;
    FakeStt* stt = nullptr; // raw observers; the engine owns the fakes
    FakeVad* vad = nullptr;
    std::shared_ptr<Tape> tape = std::make_shared<Tape>();
    std::unique_ptr<Engine> engine;

    explicit Harness(mynah::config::Config config) {
        std::filesystem::path model = dir.path() / "ggml-test-model.bin";
        {
            std::ofstream out(model, std::ios::binary);
            out << "not a real model";
        }
        config.model = model.string();

        auto stt_owner = std::make_unique<FakeStt>();
        auto vad_owner = std::make_unique<FakeVad>();
        stt = stt_owner.get();
        vad = vad_owner.get();

        Events events;
        // Init-captures: `tape` is a member, and a plain [tape] capture would
        // reach for `this`, whose lifetime outlives the engine anyway.
        events.state = [tape = tape](State state) {
            static const char* names[] = {"idle", "loading", "listening",
                                          "transcribing"};
            tape->add(std::string("state:") + names[int(state)]);
        };
        events.level = [tape = tape](double level, const float*) {
            // 33/s of these would drown the tape; a count suffices.
            tape->add("level:" + std::to_string(level));
        };
        events.text = [tape = tape](const std::string& text) { tape->add("text:" + text); };
        events.problem = [tape = tape](const char* code, const std::string& message) {
            tape->add(std::string("problem:") + code + ":" + message);
        };
        events.model = [tape = tape](ModelStatus status, const std::string& name) {
            static const char* names[] = {"loading", "loaded", "unloaded"};
            tape->add(std::string("model:") + names[int(status)] + ":" + name);
        };
        // Model lookup is pointed at this harness's temp directory, so a
        // Silero model installed on the machine running the tests cannot
        // decide whether the VAD loads.
        engine = std::make_unique<Engine>(std::move(config), std::move(stt_owner),
                                         std::move(vad_owner), std::move(events),
                                         std::vector<std::filesystem::path>{dir.path()});
    }
};

mynah::config::Config test_config() {
    mynah::config::Config config;
    config.language = "ru";
    config.auto_stop_silence = 0; // tests opt in explicitly
    config.idle_timeout = 0;      // no idle unload unless a test wants one
    config.vad = false;           // FakeVad decides
    config.frame_energy = 0.010;
    config.min_energy = 0.008;
    config.min_utterance = 0.25;
    return config;
}

// 1 s of silence (calibration), `speech_seconds` of speech, 0.9 s of
// silence to close the utterance.
std::vector<float> utterance_clip(float amplitude = 0.0625f,
                                  double speech_seconds = 1.0) {
    std::vector<float> samples;
    for (int i = 0; i < 33; ++i) samples.insert(samples.end(), 480, 0.0f);
    int speech_frames = int(speech_seconds / 0.03);
    for (int i = 0; i < speech_frames; ++i)
        samples.insert(samples.end(), 480, amplitude);
    for (int i = 0; i < 30; ++i) samples.insert(samples.end(), 480, 0.0f);
    return samples;
}

} // namespace

TEST_CASE("a session starts, transcribes, and stops") {
    Harness harness(test_config());
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    REQUIRE(harness.tape->await("model:loaded"));
    CHECK(harness.tape->contains("model:loading"));

    std::vector<float> clip = utterance_clip();
    harness.engine->push_audio(clip.data(), clip.size());
    REQUIRE(harness.tape->await("text:привет мир"));
    REQUIRE(harness.stt->calls.size() == 1);
    CHECK(harness.stt->calls[0].language == "ru");
    // The default prompt is the anti-censorship Russian prompt, which is
    // load-bearing for verbatim slang (resolve: empty prompt -> default).
    CHECK(!harness.stt->calls[0].prompt.empty());

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
    REQUIRE(harness.stt->calls.size() == 1); // nothing was pending
}

TEST_CASE("the first utterance is not spaced, the next one is") {
    Harness harness(test_config());
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));

    std::vector<float> clip = utterance_clip();
    harness.engine->push_audio(clip.data(), clip.size());
    REQUIRE(harness.tape->await("text:привет мир"));
    CHECK(harness.tape->count("text:") == 1);

    harness.engine->push_audio(clip.data(), clip.size());
    REQUIRE(harness.tape->await("text: привет мир"));
    CHECK(harness.tape->count("text:") == 2);

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("text starting with punctuation is never given a leading space") {
    Harness harness(test_config());
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));

    std::vector<float> clip = utterance_clip();
    harness.engine->push_audio(clip.data(), clip.size());
    REQUIRE(harness.tape->await("text:привет мир"));

    harness.stt->result = [] { return "…продолжение"; };
    harness.engine->push_audio(clip.data(), clip.size());
    REQUIRE(harness.tape->await("text:…продолжение"));
    CHECK(!harness.tape->contains("text: …"));

    // "—" (em dash) is NOT in the exception list ,.!?;:)]}»… — engine.py
    // gives it the space, and so must the core.
    harness.stt->result = [] { return "— тире"; };
    harness.engine->push_audio(clip.data(), clip.size());
    REQUIRE(harness.tape->await("text: — тире"));

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("a start while a model loads can be cancelled by stop") {
    Harness harness(test_config());
    // Block the fake model load on an atomic gate until the test opens it.
    auto gate = std::make_shared<std::atomic<bool>>(false);
    std::mutex release_mutex;
    std::condition_variable release_cv;
    bool open = false;
    harness.stt->on_load = [&] {
        std::unique_lock<std::mutex> lock(release_mutex);
        release_cv.wait(lock, [&] { return open || gate->load(); });
    };

    harness.engine->start();
    REQUIRE(harness.tape->await("model:loading"));
    CHECK(harness.engine->state() == State::Loading);

    // A stop during the load must cancel the start, not wait for it.
    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
    CHECK(harness.engine->state() == State::Idle);

    // Open the gate; the cancelled loader must abort WITHOUT activating.
    {
        std::lock_guard<std::mutex> lock(release_mutex);
        open = true;
    }
    release_cv.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(!harness.engine->is_capturing());
    CHECK(harness.engine->state() == State::Idle);
    CHECK(!harness.tape->contains("state:listening"));

    // A fresh start still works (the fake model is now loaded).
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("PTT press/release starts and stops; repeats and stray releases are no-ops") {
    Harness harness(test_config());
    harness.engine->ptt_press();
    REQUIRE(harness.tape->await("state:listening"));

    harness.engine->ptt_press(); // key repeat while active: ignored
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(harness.engine->state() == State::Listening);

    harness.engine->ptt_release();
    REQUIRE(harness.tape->await("state:idle"));

    harness.engine->ptt_release(); // release when idle: no-op
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(harness.engine->state() == State::Idle);
}

TEST_CASE("toggle flips the session") {
    Harness harness(test_config());
    harness.engine->toggle(); // on
    REQUIRE(harness.tape->await("state:listening"));
    harness.engine->toggle(); // off
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("an utterance shorter than min_utterance is not transcribed") {
    mynah::config::Config config = test_config();
    // The detector keeps trailing_padding (0.2 s) of silence on a closed
    // utterance (P2), so the gate reads speech + 0.2 s: 0.24 s of speech
    // closes at 0.44 s < 0.5.
    config.min_utterance = 0.5;
    Harness harness(config);
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));

    std::vector<float> clip = utterance_clip(0.0625f, 0.24);
    harness.engine->push_audio(clip.data(), clip.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(harness.stt->calls.empty());

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("a user utterance floor above the calibration cap applies exactly as set") {
    // M13's cap clamps the median's CONTRIBUTION at the speech floor
    // (0.03) — not the whole max() — so a user min_energy above the cap
    // is the gate, exactly as set. Speech whose frames clear the frame
    // floor (0.04 >= 0.010) but whose trimmed whole-buffer RMS falls
    // under the user floor (sqrt(0.04² x 0.24/0.44) = 0.0295 < 0.05) is
    // rejected — the "floors are minimums" invariant the config docs
    // promise, and the gate that keeps borderline noise out of Whisper.
    mynah::config::Config config = test_config();
    config.min_energy = 0.05;
    Harness harness(config);
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));

    std::vector<float> clip;
    for (int i = 0; i < 33; ++i) clip.insert(clip.end(), 480, 0.0f); // quiet room
    for (int i = 0; i < 8; ++i) clip.insert(clip.end(), 480, 0.04f); // 0.24 s speech
    for (int i = 0; i < 30; ++i) clip.insert(clip.end(), 480, 0.0f);
    harness.engine->push_audio(clip.data(), clip.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(harness.stt->calls.empty());

    // And a louder utterance in the same session still passes the same
    // gate — the rejection is the gate working, not the engine mute.
    clip.clear();
    for (int i = 0; i < 33; ++i) clip.insert(clip.end(), 480, 0.0f);
    for (int i = 0; i < 33; ++i) clip.insert(clip.end(), 480, 0.09f); // 1 s speech
    for (int i = 0; i < 30; ++i) clip.insert(clip.end(), 480, 0.0f);
    harness.engine->push_audio(clip.data(), clip.size());
    REQUIRE(harness.tape->await("text:привет мир"));

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("a callback may call back into the engine without deadlocking") {
    // Regression: state and level events were emitted with events_mutex_
    // held, so a front end that reacted to an event by touching the engine
    // — reading state() to refresh a menu, stop()ping on a problem — hung
    // on a non-recursive mutex, or took events_mutex_ -> mutex_ and
    // inverted the lock order. If that comes back, this test does not fail:
    // it hangs, and the timeouts below turn the hang into a failure.
    mynah_test::TmpDir dir;
    std::filesystem::path model = dir.path() / "ggml-test-model.bin";
    {
        std::ofstream out(model, std::ios::binary);
        out << "not a real model";
    }
    mynah::config::Config config = test_config();
    config.model = model.string();

    std::atomic<Engine*> engine_ptr{nullptr};
    std::atomic<int> reentrant_reads{0};
    std::atomic<bool> stopped_from_callback{false};

    Events events;
    events.state = [&](State state) {
        Engine* engine = engine_ptr.load();
        if (!engine) return;
        (void)engine->state(); // re-entrant read: the deadlock that was
        reentrant_reads.fetch_add(1);
        // And a re-entrant command, the thing a front end really does.
        if (state == State::Listening && !stopped_from_callback.exchange(true))
            engine->stop();
    };
    events.level = [&](double, const float*) {
        if (Engine* engine = engine_ptr.load()) {
            (void)engine->state();
            reentrant_reads.fetch_add(1);
        }
    };

    auto engine = std::make_unique<Engine>(
        config, std::make_unique<FakeStt>(), std::make_unique<FakeVad>(),
        std::move(events), std::vector<std::filesystem::path>{dir.path()});
    engine_ptr.store(engine.get());

    engine->start();
    bool idle_again = false;
    for (int i = 0; i < 1000 && !idle_again; ++i) {
        idle_again = stopped_from_callback.load() && engine->state() == State::Idle;
        if (!idle_again) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(idle_again); // the callback's stop() went through
    CHECK(reentrant_reads.load() > 0);

    engine_ptr.store(nullptr); // the callbacks outlive nothing they touch
}

TEST_CASE("the Silero model is loaded when vad is on") {
    // Regression: nothing ever called vad->load(), so contains_speech()
    // always took its fail-open path — "vad = on" checked nothing at all,
    // and the second stage that rejects fan noise before Whisper can
    // hallucinate on it was silently absent.
    mynah::config::Config config = test_config();
    config.vad = true;
    Harness harness(config);
    {
        std::ofstream out(harness.dir.path() / "ggml-silero-v5.1.2.bin", std::ios::binary);
        out << "not a real vad model";
    }

    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    CHECK(harness.vad->is_loaded());
    CHECK(!harness.tape->contains("problem:vad_degraded"));

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("a missing Silero model degrades loudly, and dictation still works") {
    // Fail-open is the rule — a VAD that cannot run must never swallow
    // speech — but it must SAY so, or "vad = on" reads as working while
    // nothing is being checked.
    mynah::config::Config config = test_config();
    config.vad = true;
    Harness harness(config); // no silero model in the harness directory

    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    REQUIRE(harness.tape->await("problem:vad_degraded"));
    CHECK(!harness.vad->is_loaded());

    std::vector<float> clip = utterance_clip();
    harness.engine->push_audio(clip.data(), clip.size());
    REQUIRE(harness.tape->await("text:привет мир")); // fail-open: still typed

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("vad off never loads the model, and never complains") {
    mynah::config::Config config = test_config();
    config.vad = false;
    Harness harness(config);

    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    CHECK(!harness.vad->is_loaded());
    CHECK(!harness.tape->contains("problem:vad_degraded"));

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("VAD rejects a clip with no voice in it") {
    mynah::config::Config config = test_config();
    config.vad = true;
    Harness harness(config);
    harness.vad->speech.store(false);
    harness.engine->start();
    // The session loads the VAD at start (fail-open if it cannot).
    REQUIRE(harness.tape->await("state:listening"));

    std::vector<float> clip = utterance_clip();
    harness.engine->push_audio(clip.data(), clip.size());
    // Transcribing happened, then the clip was rejected: back to
    // listening, nothing typed, STT never called.
    REQUIRE(harness.tape->await_count("state:listening", 2));
    REQUIRE(harness.tape->await("state:transcribing"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(harness.stt->calls.empty());
    CHECK(!harness.tape->contains("text:"));

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("a hallucination is dropped, never typed") {
    // "спасибо за просмотр" is an artifact phrase — matched by substring
    // anywhere (NS-6). Vocab words like "субтитры" only match as the whole
    // utterance; a longer sentence containing them MUST type.
    Harness harness(test_config());
    harness.stt->result = [] { return "Ну и спасибо за просмотр, друзья"; };
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    std::vector<float> clip = utterance_clip();
    harness.engine->push_audio(clip.data(), clip.size());
    // The second "listening" (after transcribing) is the worker finishing:
    // the STT was called and its result dropped. No text event may exist.
    REQUIRE(harness.tape->await_count("state:listening", 2, 20000));
    REQUIRE(harness.stt->calls.size() == 1);
    CHECK(!harness.tape->contains("text:"));
    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("queued utterances merge into one transcription") {
    Harness harness(test_config());
    // A latch, not a sleep: the first transcription holds open until the
    // test has queued #2 and #3, so the merge is exercised deterministically
    // no matter how the scheduler interleaves the threads.
    std::atomic<bool> hold_first{true};
    harness.stt->result = [&hold_first] {
        while (hold_first.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return "привет мир";
    };
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));

    std::vector<float> clip = utterance_clip();
    harness.engine->push_audio(clip.data(), clip.size()); // #1 picked up
    REQUIRE(harness.tape->await("state:transcribing"));
    harness.engine->push_audio(clip.data(), clip.size()); // #2 queued
    harness.engine->push_audio(clip.data(), clip.size()); // #3 queued

    // Wait for the audio worker to have CLOSED #2 and #3, rather than
    // sleeping and hoping. The audio worker emits exactly one level event
    // per processed frame, so once three clips' worth of frames have been
    // reported, every frame is through the detector and both utterances
    // are on the queue. A sleep here was a timing bet that lost whenever
    // the machine was loaded or instrumented — it failed under TSan.
    const int frames_per_clip = int(clip.size() / 480);
    REQUIRE(harness.tape->await_count("level:", frames_per_clip * 3, 30000));
    hold_first.store(false, std::memory_order_release);

    // #1 transcribes alone; #2 + #3 merge into one call. 3 utterances,
    // 2 transcriptions, in spoken order. The sample counts are of the
    // TRIMMED utterances (speech + trailing_padding — the P2 policy),
    // not of the input clips, so the assertions are relative to whatever
    // call[0] actually carried.
    REQUIRE(harness.tape->await("text:привет мир", 20000));
    REQUIRE(harness.tape->await("text: привет мир", 20000));
    REQUIRE(harness.stt->calls.size() == 2);
    std::size_t single = harness.stt->call_sample_counts[0];
    std::size_t merged_count = harness.stt->call_sample_counts[1];
    CHECK(single > 480 * 30);        // the 1 s of speech, not a fragment
    CHECK(merged_count > single * 3 / 2); // two trimmed utterances
    CHECK(merged_count < single * 3);

    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}

TEST_CASE("auto-stop ends the session after continuous silence") {
    mynah::config::Config config = test_config();
    config.auto_stop_silence = 0.5;
    Harness harness(config);
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));

    // Silence only: nobody ever speaks. 0.5 s of it must end the session
    // on its own, without an explicit stop.
    std::vector<float> silence;
    for (int i = 0; i < 34; ++i) silence.insert(silence.end(), 480, 0.0f);
    harness.engine->push_audio(silence.data(), silence.size());
    REQUIRE(harness.tape->await("state:idle"));
    CHECK(!harness.engine->is_capturing());
}

TEST_CASE("speech resets the auto-stop clock") {
    mynah::config::Config config = test_config();
    config.auto_stop_silence = 0.5;
    Harness harness(config);
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));

    // Silence 0.3 s, speech 0.3 s, silence 0.3 s, speech 0.3 s, then 0.9 s
    // of silence to close the utterance. Every gap is under the 0.5 s
    // budget, so the session must survive them all and transcribe the
    // utterance — only the final, utterance-closing silence is long
    // enough to auto-stop, and stopping then is exactly right.
    std::vector<float> clip;
    for (int i = 0; i < 10; ++i) clip.insert(clip.end(), 480, 0.0f);
    for (int i = 0; i < 10; ++i) clip.insert(clip.end(), 480, 0.0625f);
    for (int i = 0; i < 10; ++i) clip.insert(clip.end(), 480, 0.0f);
    for (int i = 0; i < 10; ++i) clip.insert(clip.end(), 480, 0.0625f);
    for (int i = 0; i < 30; ++i) clip.insert(clip.end(), 480, 0.0f);
    harness.engine->push_audio(clip.data(), clip.size());
    bool got_text = harness.tape->await("text:привет мир", 10000);
    if (!got_text) {
        std::string joined;
        for (const std::string& line : harness.tape->take()) joined += line + "\n";
        REQUIRE_MESSAGE(false, ("no text arrived; tape:\n" + joined));
    }
    REQUIRE(harness.tape->await("state:idle"));
    CHECK(!harness.engine->is_capturing());
}

TEST_CASE("the model unloads after idle_timeout; a start cancels the timer") {
    mynah::config::Config config = test_config();
    config.idle_timeout = 0.2;
    Harness harness(config);
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
    CHECK(harness.stt->is_loaded());

    REQUIRE(harness.tape->await("model:unloaded"));
    CHECK(!harness.stt->is_loaded());

    // Start again: the timer must cancel, the model reloads.
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    REQUIRE(harness.tape->await("model:loaded"));
    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
    REQUIRE(harness.tape->await("model:unloaded"));
}

TEST_CASE("destroy drains a pending utterance before returning") {
    Harness harness(test_config());
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    // An utterance closes; do NOT stop — destroy must handle the rest.
    std::vector<float> clip = utterance_clip();
    harness.engine->push_audio(clip.data(), clip.size());
    REQUIRE(harness.tape->await("state:transcribing"));
    harness.engine.reset(); // ~Engine: stop, join, unload
    CHECK(harness.tape->contains("text:привет мир"));
}

TEST_CASE("audio pushed while idle is dropped, and never leaks into the next session") {
    Harness harness(test_config());
    std::vector<float> clip = utterance_clip();
    harness.engine->push_audio(clip.data(), clip.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(harness.engine->state() == State::Idle);

    // The next session must not see any of it: the loader drains the ring.
    harness.engine->start();
    REQUIRE(harness.tape->await("state:listening"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(harness.stt->calls.empty());
    harness.engine->stop();
    REQUIRE(harness.tape->await("state:idle"));
}