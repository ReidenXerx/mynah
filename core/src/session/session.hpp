// mynah::session — the dictation session state machine.
//
// The synthesis of the two engines this core replaces:
//
//   - Python's engine.py contributes: the session generation counter (the
//     zombie-session race where a stale teardown poisons the new
//     session's queue), the utterance queue with merge ≤ 20 s, the spacing
//     rule between utterances, auto-stop after silence, and the 45 s idle
//     unload.
//   - Swift's SessionController contributes: an asynchronous start (a cold
//     model load must never block the caller), cancelling a start while
//     the model loads (the generation counter again, this time on the
//     start path), and returning from stop at once — remaining work is
//     reported as events, not joined.
//
// P2/P4 policy: the detector trims trailing silence to trailing_padding
// (Swift's segmentation) and min_utterance applies to the trimmed buffer;
// the per-utterance gates (min_utterance, whole-buffer RMS vs the
// calibrated gate) run at enqueue, before the merge — each merged clip is
// made of verified speech, and a quiet blip cannot ride a louder
// utterance in.
//
// Threading (the C API contract):
//   - push_audio is called from the capture thread and only touches the
//     SPSC ring buffer.
//   - One audio worker per session pops the ring, runs the detector and
//     the level/spectrum events, applies the enqueue gates, and owns
//     auto-stop.
//   - One transcribe worker per session drains the queue: merge, Silero
//     VAD (fail-open), whisper.cpp, the hallucination filter, the spacing
//     rule, the TEXT event.
//   - A loader thread runs model loads for start(); an idle-unload thread
//     runs after stop().
//   - Events are delivered on these threads and must not block.
//
// No user callback is ever invoked with one of these locks held. That is a
// rule, not an accident: a front end reacting to an event by calling back
// into the engine (a "no_model" problem that stops the session, a state
// change that reads state()) would otherwise deadlock on a non-recursive
// mutex, or take events_mutex_ → mutex_ and invert the order below. The
// cost is that two events can be delivered concurrently by different
// threads; front ends that need ordering marshal to their own thread
// anyway (Swift's MainActor, Qt's queued connections).
//
// Lock order: mutex_ (lifecycle) → events_mutex_ (state). No thread ever
// takes them the other way around.

#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/analysis.hpp"
#include "audio/ring_buffer.hpp"
#include "config/config.hpp"
#include "segment/utterance_detector.hpp"
#include "stt/stt.hpp"
#include "vad/vad.hpp"

namespace mynah::session {

enum class State { Idle, Loading, Listening, Transcribing };
enum class ModelStatus { Loading, Loaded, Unloaded };

// What the session reports out; the C API wraps these into mynah_event.
// All callbacks run on engine threads and must not block. Any may be null.
struct Events {
    std::function<void(State)> state;
    // level 0..1 and MYNAH_SPECTRUM_BANDS band values 0..1.
    std::function<void(double level, const float* bands)> level;
    // The final text, spacing already applied — the front end types it.
    std::function<void(const std::string& text)> text;
    // Stable problem codes: "no_model", "model_load_failed",
    // "transcription_failed", "vad_degraded".
    std::function<void(const char* code, const std::string& message)> problem;
    std::function<void(ModelStatus, const std::string& name)> model;
};

class Engine {
public:
    // `model_dirs` is where the Whisper and Silero models are looked for;
    // empty means models::search_directories(), which is what every front
    // end passes. The tests point it at a temp directory so what is
    // installed on the machine running them cannot change the result.
    Engine(config::Config config, std::unique_ptr<stt::SpeechToText> stt,
           std::unique_ptr<vad::VoiceActivity> vad, Events events,
           std::vector<std::filesystem::path> model_dirs = {});
    ~Engine(); // stops, joins every thread, frees the model and VAD

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // New config takes effect on the next session (the live session's
    // calibration and queue belong to it).
    void set_config(config::Config config);

    // The config the engine is currently running with. Thread-safe: a front
    // end reads this while the session lifecycle mutates it.
    config::Config config_snapshot() const;

    // Trigger semantics: toggle flips, PTT press/release start and stop.
    void toggle();
    void start(); // asynchronous; LOADING..LISTENING arrive as events
    void stop();  // returns at once; draining is reported as events
    void ptt_press();
    void ptt_release();

    // Capture thread only. Takes no engine lock and allocates nothing: the
    // samples go into the lock-free ring, which drops what does not fit,
    // and the worker is woken best-effort. A wakeup that races the
    // worker's sleep costs latency, never audio — the worker also wakes on
    // a timer (see kWakeInterval in the .cpp).
    void push_audio(const float* samples, std::size_t count);

    State state() const;
    bool is_model_loaded() const;
    bool is_capturing() const { return capturing_.load(std::memory_order_acquire); }

    // Diagnostics for tests and front ends.
    std::uint64_t dropped_samples() const { return ring_.dropped(); }

private:
    // A jthread that flags itself done on exit. The engine keeps a list
    // and reaps finished ones opportunistically — replacing a live thread
    // must never join it (start/stop stay non-blocking).
    //
    // Held by shared_ptr because a Session also refers to its two workers:
    // reaping `threads_` used to free the OwnedThread while the Session
    // still pointed at it, and the very next line read `->done` through
    // that freed pointer (a heap-use-after-free ASan caught in the fuzz
    // suite). Sharing ownership means whichever list drops its reference
    // last does the freeing, in any order.
    struct OwnedThread {
        std::jthread thread;
        std::atomic<bool> done{false};
    };

    // A closed utterance waiting for the transcribe worker. flush() marks
    // the queue closed: no more input, drain and exit.
    struct UtteranceQueue {
        void push(segment::Utterance utterance);
        void flush();
        // Blocking pop; nullopt once the queue is flushed AND empty.
        std::optional<segment::Utterance> pop();
        std::optional<segment::Utterance> try_pop(); // nullopt when empty
        bool closed_and_empty() const;

    private:
        mutable std::mutex mutex; // closed_and_empty() is a const reader
        std::condition_variable readable;
        std::deque<segment::Utterance> items;
        bool flushed = false;
    };

    // Everything a session's workers own, bound to the generation that
    // started them. A stale session drains itself and never touches the
    // new one's state (engine.py's W2-H1).
    struct Session {
        UtteranceQueue queue;
        std::shared_ptr<OwnedThread> audio;
        std::shared_ptr<OwnedThread> transcribe;
    };

    void spawn_loader(int start_generation);
    void loader_body(int start_generation);
    void audio_worker(std::shared_ptr<Session> session, config::Config cfg, int generation);
    void transcribe_worker(std::shared_ptr<Session> session, config::Config cfg, int generation);
    void schedule_idle_unload(); // NOT called under mutex_/events_mutex_
    void set_state(State next);
    void emit_problem(const char* code, const std::string& message);
    bool is_engaged() const; // active, or a start still loading
    static std::string spaced(const std::string& text, bool typed_in_session);
    void reap_finished_threads(); // with mutex_ held; joins done threads only

    // Immutable after construction.
    std::vector<std::filesystem::path> model_dirs_;
    audio::Spectrum spectrum_;
    std::unique_ptr<stt::SpeechToText> stt_;
    std::unique_ptr<vad::VoiceActivity> vad_;
    Events events_;

    audio::RingBuffer ring_{10 * 16000};
    mutable std::mutex events_mutex_; // state() and set_state; const readers lock it
    State state_ = State::Idle;

    mutable std::mutex mutex_; // lifecycle bookkeeping below (is_engaged() is const)
    config::Config config_;
    std::atomic<bool> active_{false};    // a live session
    std::atomic<bool> start_pending_{false}; // a start still loading the model
    std::atomic<int> start_generation_{0};  // guards which start finishes
    std::atomic<int> session_generation_{0}; // guards teardown vs restart
    std::atomic<int> idle_generation_{0};    // cancels the idle timer
    std::atomic<bool> quitting_{false};
    std::vector<std::shared_ptr<OwnedThread>> threads_;
    std::vector<std::shared_ptr<Session>> sessions_; // newest last
    std::vector<std::shared_ptr<Session>> stale_sessions_; // finished or draining

    // The audio worker's wakeup. Separate from the idle timer's below:
    // sharing one condition variable meant a push could wake the timer
    // instead of the worker (notify_one picks a waiter, not a purpose).
    std::mutex push_mutex_;
    std::condition_variable push_cv_;
    std::atomic<bool> capturing_{false};

    // The idle-unload timer's wakeup, so a cancelled timer is woken by
    // start/stop without touching the audio path.
    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
};

} // namespace mynah::session