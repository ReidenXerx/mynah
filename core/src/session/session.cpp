#include "session.hpp"

#include <chrono>
#include <filesystem>
#include <utility>

#include "filter/transcript_filter.hpp"
#include "models/resolve.hpp"
#include "tuning/constants.hpp"

namespace mynah::session {

namespace {

constexpr std::size_t kMergeLimitSamples =
    std::size_t(constants::merge_limit_seconds * 16000);

// How long the audio worker sleeps before looking at the ring again even
// when nobody woke it. The capture thread notifies WITHOUT holding
// push_mutex_ — it must not block, and taking the worker's lock on the
// real-time thread is exactly the blocking the C API contract forbids —
// so a notify can land in the window between the worker's predicate check
// and its sleep and be missed. This bound turns that from a hang into a
// one-frame-ish delay: 20 ms is under the 30 ms frame the engine works in,
// so a missed wakeup never stalls segmentation.
constexpr auto kWakeInterval = std::chrono::milliseconds(20);

// The spacing rule's leading-punctuation exception, UTF-8-aware: the rule
// looks at the FIRST CHARACTER, and two of the characters ("»", "…") are
// multi-byte. engine.py's comparison is character-wise; a byte-wise find
// would treat any text starting with 0xC2 (Â, ¡, ¢…) as punctuation.
bool starts_with_leading_punct(const std::string& text) {
    if (text.empty()) return false;
    unsigned char lead = static_cast<unsigned char>(text[0]);
    if (lead < 0x80)
        return std::string_view(",.!?;:)]}").find(char(text[0])) != std::string_view::npos;
    if (lead == 0xC2 && text.size() >= 2 && static_cast<unsigned char>(text[1]) == 0xBB)
        return true; // »
    if (lead == 0xE2 && text.size() >= 3 && static_cast<unsigned char>(text[1]) == 0x80 &&
        static_cast<unsigned char>(text[2]) == 0xA6)
        return true; // …
    return false;
}

} // namespace

// --- UtteranceQueue -----------------------------------------------------------

void Engine::UtteranceQueue::push(segment::Utterance utterance) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        items.push_back(std::move(utterance));
    }
    readable.notify_one();
}

void Engine::UtteranceQueue::flush() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        flushed = true;
    }
    readable.notify_all();
}

std::optional<segment::Utterance> Engine::UtteranceQueue::pop() {
    std::unique_lock<std::mutex> lock(mutex);
    readable.wait(lock, [this] { return flushed || !items.empty(); });
    if (items.empty()) return std::nullopt; // flushed and drained
    segment::Utterance utterance = std::move(items.front());
    items.pop_front();
    return utterance;
}

std::optional<segment::Utterance> Engine::UtteranceQueue::try_pop() {
    std::lock_guard<std::mutex> lock(mutex);
    if (items.empty()) return std::nullopt;
    segment::Utterance utterance = std::move(items.front());
    items.pop_front();
    return utterance;
}

bool Engine::UtteranceQueue::closed_and_empty() const {
    std::lock_guard<std::mutex> lock(mutex);
    return flushed && items.empty();
}

// --- lifecycle -----------------------------------------------------------------

Engine::Engine(config::Config config, std::unique_ptr<stt::SpeechToText> stt,
               std::unique_ptr<vad::VoiceActivity> vad, Events events,
               std::vector<std::filesystem::path> model_dirs)
    : model_dirs_(model_dirs.empty() ? models::search_directories() : std::move(model_dirs)),
      spectrum_(audio::kFrameSamples), stt_(std::move(stt)), vad_(std::move(vad)),
      events_(std::move(events)), config_(std::move(config)) {}

Engine::~Engine() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quitting_.store(true, std::memory_order_release);
    }
    stop();
    ++idle_generation_; // cancel the idle timer
    {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        idle_cv_.notify_all();
    }
    push_cv_.notify_all(); // and any audio worker still sleeping
    // Join everything WITHOUT holding mutex_: the loader and worker exit
    // paths take it, and joining under the lock would wait on a thread that
    // is itself waiting for the lock. The destructor has no concurrent
    // callers — destroy happens after every other call.
    for (auto& owned : threads_)
        if (owned->thread.joinable()) owned->thread.join();
    threads_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
        stale_sessions_.clear();
    }
    // The C API's destroy contract: whisper and VAD contexts are freed
    // before mynah_destroy returns — ggml aborts at exit if a Metal
    // context is still alive (docs/SWIFT-APP.md, open issue 9).
    stt_->unload();
    vad_->unload();
}

void Engine::reap_finished_threads() {
    // With mutex_ held. Joining a thread that set done is instant; one
    // that is still running is left alone — never waited on here.
    std::erase_if(threads_, [](const std::shared_ptr<OwnedThread>& owned) {
        if (!owned->done.load(std::memory_order_acquire)) return false;
        if (owned->thread.joinable()) owned->thread.join();
        return true;
    });
    auto workers_finished = [](const std::shared_ptr<Session>& session) {
        return session->audio && session->transcribe &&
               session->audio->done.load(std::memory_order_acquire) &&
               session->transcribe->done.load(std::memory_order_acquire);
    };
    std::erase_if(sessions_, workers_finished);
    std::erase_if(stale_sessions_, workers_finished);
}

void Engine::set_config(config::Config config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = std::move(config);
}

config::Config Engine::config_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool Engine::is_engaged() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_.load(std::memory_order_acquire) ||
           start_pending_.load(std::memory_order_acquire);
}

State Engine::state() const {
    std::lock_guard<std::mutex> lock(events_mutex_);
    return state_;
}

bool Engine::is_model_loaded() const { return stt_->is_loaded(); }

void Engine::set_state(State next) {
    // The transition is decided under the lock; the callback runs after it
    // is released. events_ is immutable after construction, so reading it
    // unlocked is safe — and a front end that calls back into the engine
    // from this callback (stop() on a problem, state() to refresh a menu)
    // finds no lock of ours held.
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        if (state_ == next) return;
        state_ = next;
    }
    if (events_.state) events_.state(next);
}

void Engine::emit_problem(const char* code, const std::string& message) {
    if (events_.problem) events_.problem(code, message);
}

// --- trigger -------------------------------------------------------------------

void Engine::toggle() {
    if (is_engaged()) stop();
    else start();
}

void Engine::ptt_press() {
    if (is_engaged()) return; // already active — ignore key repeat
    start();
}

void Engine::ptt_release() {
    if (!is_engaged()) return;
    stop();
}

void Engine::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_.load(std::memory_order_acquire) ||
            start_pending_.load(std::memory_order_acquire))
            return; // rapid double-press
        // Cancel any pending idle unload — we are active again.
        ++idle_generation_;
        start_pending_.store(true, std::memory_order_release);
        ++start_generation_;
        // Capture from the press, not from LISTENING: a cold model load is
        // seconds (turbo onto a dGPU woken from D3cold: 3.2 s), and people
        // start talking when they press the key. What they say meanwhile
        // waits in the ring for the session's worker. The session's
        // generation advances here too, so an earlier session's audio
        // worker stops reading the ring now; the position is taken before
        // capture is armed, so every sample after it is this session's.
        ++session_generation_;
        pending_capture_from_ = ring_.written();
        capturing_.store(true, std::memory_order_release);
        spawn_loader(start_generation_.load());
        reap_finished_threads();
    }
    {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        idle_cv_.notify_all(); // wake the idle timer so it sees the cancel
    }
}

void Engine::spawn_loader(int start_generation) {
    // With mutex_ held.
    auto owned = std::make_shared<OwnedThread>();
    owned->thread = std::jthread([this, start_generation] {
        loader_body(start_generation);
    });
    threads_.push_back(std::move(owned));
}

// The start path: load, then activate — or abort, when a stop arrived
// while the model loaded. start_generation_ distinguishes THIS start from
// a newer one (cancel-then-immediately-restart); session_generation_
// distinguishes this session from a teardown racing it (engine.py's
// W2-H1, both halves).
void Engine::loader_body(int start_generation) {
    config::Config cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quitting_.load(std::memory_order_acquire) ||
            !start_pending_.load(std::memory_order_acquire) ||
            start_generation_.load() != start_generation)
            return;
        cfg = config_;
    }

    // Resolved even when the model is already loaded: the VAD model is
    // looked for beside it before the shared search directories.
    std::filesystem::path model =
        models::resolve(cfg.model, model_dirs_, stt_->gpu_available(cfg.gpu));
    bool need_load = !stt_->is_loaded();
    if (need_load) {
        if (model.empty()) {
            // No model anywhere: surface it immediately rather than after
            // the user has spoken a whole sentence into a dead session.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (start_generation_.load() == start_generation) {
                    start_pending_.store(false, std::memory_order_release);
                    capturing_.store(false, std::memory_order_release);
                }
            }
            set_state(State::Idle);
            emit_problem("no_model",
                         "No Whisper model found. Download one (macOS: Settings → "
                         "Recognition; Linux: mynah models download).");
            schedule_idle_unload();
            return;
        }
        set_state(State::Loading);
        if (events_.model) events_.model(ModelStatus::Loading, model.filename().string());
        if (!stt_->load(model, cfg.gpu)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (start_generation_.load() == start_generation) {
                    start_pending_.store(false, std::memory_order_release);
                    capturing_.store(false, std::memory_order_release);
                }
            }
            set_state(State::Idle);
            emit_problem("model_load_failed",
                         "Could not load Whisper model '" + model.filename().string() + "'.");
            schedule_idle_unload();
            return;
        }
        if (events_.model) events_.model(ModelStatus::Loaded, model.filename().string());
    }

    // Silero, the second stage of speech detection: the energy gates only
    // measure loudness, so a fan or a keyboard passes them as readily as a
    // voice — and near-silent noise handed to Whisper is exactly what
    // produces its subtitle-credit hallucinations. Loaded here, on the
    // loader thread, because it is a file read and a model init like the
    // Whisper one; ~0.8 MB, so it is cheap next to the model above.
    //
    // FAIL-OPEN, and say so: a missing or unloadable VAD model must never
    // silently swallow utterances, but "vad = on" reading as if it were
    // working while nothing is checked is the failure the front ends need
    // to be able to show.
    if (cfg.vad && !vad_->is_loaded()) {
        std::vector<std::filesystem::path> vad_dirs;
        if (!model.empty()) vad_dirs.push_back(model.parent_path());
        for (const auto& dir : model_dirs_) vad_dirs.push_back(dir);
        std::filesystem::path vad_model = models::resolve_vad(vad_dirs);
        if (vad_model.empty()) {
            emit_problem("vad_degraded",
                         "No Silero VAD model found — loudness-only rejection for this "
                         "session. Download it (macOS: Settings → Recognition; Linux: "
                         "mynah models download-vad).");
        } else if (!vad_->load(vad_model)) {
            emit_problem("vad_degraded", "Could not load the Silero VAD model '" +
                                             vad_model.filename().string() +
                                             "' — loudness-only rejection for this session.");
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The user may have pressed the trigger again while the model
        // loaded: abort cleanly (Swift's Task.isCancelled checks — the
        // second press must cancel a cold load, not be ignored).
        if (quitting_.load(std::memory_order_acquire) ||
            !start_pending_.load(std::memory_order_acquire) ||
            start_generation_.load() != start_generation)
            return; // the stop path already set the state
        start_pending_.store(false, std::memory_order_release);
        active_.store(true, std::memory_order_release);

        // Fresh per-session state: re-measure the ambient noise floor
        // every session (a room that got louder between sessions must be
        // handled), and nothing typed yet, so the first utterance is not
        // spaced away from whatever the user was already writing. The
        // generation was taken, and capture armed, by start() — at the
        // press; a newer start would have failed the check above.
        int generation = session_generation_.load();
        auto session = std::make_shared<Session>();
        // Stale audio from a previous session must not reach this one's
        // detector. Not drained here: that would be a second reader on a
        // single-consumer ring while an earlier worker may still be
        // mid-pop. This session's worker skips to capture_from instead,
        // once those workers are gone.
        session->capture_from = pending_capture_from_;
        for (const auto* list : {&sessions_, &stale_sessions_})
            for (const auto& earlier : *list)
                if (earlier->audio && !earlier->audio->done.load(std::memory_order_acquire))
                    session->predecessors.push_back(earlier->audio);
        sessions_.push_back(session);

        auto spawn = [&](void (Engine::*body)(std::shared_ptr<Session>, config::Config, int),
                         std::shared_ptr<OwnedThread>* slot) {
            auto owned = std::make_shared<OwnedThread>();
            // The body captures a RAW pointer to its own OwnedThread: a
            // shared_ptr there would keep the thread object alive through
            // itself and it could never be reaped. The two lists below own
            // it, and both outlive the body.
            OwnedThread* raw = owned.get();
            owned->thread = std::jthread([this, body, session, cfg, generation, raw] {
                (this->*body)(session, cfg, generation);
                raw->done.store(true, std::memory_order_release);
            });
            *slot = owned;
            threads_.push_back(std::move(owned));
        };
        spawn(&Engine::audio_worker, &session->audio);
        spawn(&Engine::transcribe_worker, &session->transcribe);
    }
    set_state(State::Listening);
    push_cv_.notify_all();

    // A model that was already loaded may sit on a GPU that has gone to
    // sleep since (a dGPU in D3cold ten seconds after its last work): wake
    // it now, while the user is still on their first sentence, rather than
    // when that sentence is handed over — ~1 s it would otherwise add to
    // the first text. A model loaded just now needs nothing: the load woke
    // the device.
    if (!need_load) stt_->wake();
}

void Engine::stop() {
    std::shared_ptr<Session> current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (start_pending_.load(std::memory_order_acquire)) {
            // Cancel a start that has not completed yet, so a second press
            // of the trigger aborts a cold load rather than being ignored
            // (Swift's endSession). The loader sees the cleared flag after
            // its load and never activates; a model it loaded mid-flight
            // is owned by the idle timer below. Capture was armed at the
            // press; what it gathered is skipped by the next session.
            start_pending_.store(false, std::memory_order_release);
            capturing_.store(false, std::memory_order_release);
        } else if (!active_.load(std::memory_order_acquire)) {
            return; // idempotent: stop on an idle engine is a no-op
        } else {
            active_.store(false, std::memory_order_release);
            capturing_.store(false, std::memory_order_release);
            // The session's workers own its queue; they drain themselves.
            // Keep the shared_ptr until they finish reaping.
            if (!sessions_.empty()) {
                current = sessions_.back();
                stale_sessions_.push_back(current);
                std::erase(sessions_, current);
            }
        }
        reap_finished_threads();
    }

    set_state(State::Idle);
    push_cv_.notify_all(); // the audio worker: it sees the flag and flushes
    {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        idle_cv_.notify_all();
    }
    schedule_idle_unload();
}

// --- audio -----------------------------------------------------------------------

void Engine::push_audio(const float* samples, std::size_t count) {
    if (!capturing_.load(std::memory_order_acquire)) return;
    // Lock-free: the ring is SPSC and this is the producer. No mutex is
    // taken here — the capture thread must never wait on a worker — so the
    // notify below is best-effort and the worker's bounded sleep
    // (kWakeInterval) is what makes a missed one harmless.
    ring_.push(samples, count);
    push_cv_.notify_one();
}

void Engine::audio_worker(std::shared_ptr<Session> session, config::Config cfg, int generation) {
    // capturing_ is engine-wide, so it is not enough on its own: a
    // stop()+start() pair inside this worker's wakeup window would re-arm
    // the flag before the loop re-read it, leaving a stale worker popping
    // the ring alongside the new session's worker — two consumers on an
    // SPSC buffer, and the new session quietly missing the audio the old
    // one swallowed. The generation says which session this thread belongs
    // to and never comes back.
    auto still_mine = [this, generation] {
        return capturing_.load(std::memory_order_acquire) &&
               session_generation_.load(std::memory_order_acquire) == generation;
    };
    segment::UtteranceDetector detector(audio::kSampleRate, cfg.frame_energy, cfg.min_energy);
    std::vector<float> frame;
    frame.reserve(audio::kFrameSamples);
    std::vector<float> scratch(audio::kFrameSamples);
    float bands[constants::spectrum_bands];

    // transcription_mode = "on_stop" (Dudu's request): the whole session
    // lands in one buffer and is transcribed once, when the session ends —
    // one coherent decode with full context. The Python engine's vad=false
    // mode was exactly this. The detector still runs per frame: it drives
    // the level meter's gates and auto-stop; only its utterance output is
    // discarded, and every frame lands in the batch instead.
    const bool batch_mode = cfg.transcription_mode == "on_stop";
    std::vector<float> batch;

    auto accept = [&](segment::Utterance& utterance) {
        // The enqueue gates (Swift's placement): min_utterance on the
        // TRIMMED duration (P2), and the whole-buffer RMS against the
        // CALIBRATED gate — never the static floor alone. In batch mode the
        // "utterance" is the whole session, which is the Python engine's
        // no-VAD behaviour verbatim; note the trade it inherits: silence
        // dilutes the batch RMS, so a long session with little talking can
        // fall under the gate.
        if (utterance.duration < cfg.min_utterance) return;
        if (audio::rms(utterance.samples.data(), utterance.samples.size()) <
            detector.current_energy_threshold())
            return;
        session->queue.push(std::move(utterance));
    };

    auto process_frame = [&](const float* samples, std::size_t count) {
        double frame_rms = audio::rms(samples, count);
        spectrum_.compute(samples, count, bands);
        // Emitted without events_mutex_: that lock guards `state_`, not the
        // callbacks, and holding it across a front end's code is what made
        // a callback that touched the engine deadlock.
        if (events_.level) events_.level(audio::level(frame_rms), bands);
        if (batch_mode) {
            // The detector's per-frame state keeps the level gates and
            // auto-stop honest; its utterance output is discarded — the
            // batch buffer is what gets transcribed.
            batch.insert(batch.end(), samples, samples + count);
            (void)detector.process(samples, count);
        } else if (auto utterance = detector.process(samples, count)) {
            accept(*utterance);
        }

        // Auto-stop after prolonged silence with no utterance open. Safe
        // from this thread: stop() never joins, it only flips flags.
        if (cfg.auto_stop_silence > 0 && !detector.is_currently_speaking() &&
            detector.continuous_silence() >= cfg.auto_stop_silence)
            stop();
    };

    // Become the ring's only reader: the audio workers of earlier sessions
    // stop on their own now that the generation has moved (within
    // kWakeInterval), and until they have, this worker neither reads nor
    // skips. Then drop what was written before this session's press.
    bool sole_reader = true;
    for (const auto& earlier : session->predecessors)
        while (!earlier->done.load(std::memory_order_acquire)) {
            if (!still_mine()) {
                sole_reader = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    if (sole_reader) ring_.skip_to(session->capture_from);

    while (sole_reader && still_mine()) {
        std::size_t popped = ring_.pop(scratch.data(), scratch.size());
        if (popped == 0) {
            std::unique_lock<std::mutex> lock(push_mutex_);
            push_cv_.wait_for(lock, kWakeInterval, [this, &still_mine] {
                return !still_mine() || quitting_.load(std::memory_order_acquire) ||
                       ring_.readable() > 0;
            });
            continue;
        }
        frame.insert(frame.end(), scratch.begin(), scratch.begin() + popped);
        while (frame.size() >= audio::kFrameSamples) {
            process_frame(frame.data(), audio::kFrameSamples);
            frame.erase(frame.begin(), frame.begin() + audio::kFrameSamples);
        }
    }

    // The session ended: close out whatever is buffered. A partial frame
    // (the capture thread's last sub-30 ms) is dropped — both prior
    // engines consume whole frames only. In batch mode the WHOLE session
    // buffer is the utterance: it goes through the same gates as a live
    // one, then one decode.
    if (batch_mode) {
        if (!batch.empty()) {
            // The duration is computed BEFORE the move: initializers run in
            // order, and a moved-from vector's size is 0 — the first version
            // of this line read the size after the move and every batch was
            // rejected by the min_utterance gate as a zero-second utterance.
            double duration = double(batch.size()) / audio::kSampleRate;
            segment::Utterance whole{std::move(batch), duration};
            accept(whole);
        }
    } else if (auto final_utterance = detector.flush()) {
        accept(*final_utterance);
    }
    session->queue.flush();
}

// --- transcription ----------------------------------------------------------------

void Engine::transcribe_worker(std::shared_ptr<Session> session, config::Config cfg, int generation) {
    // Nothing typed yet, per session: the first utterance is not spaced
    // away from whatever the user was already writing.
    bool typed_in_session = false;
    std::vector<float> merged;

    // A stale session's worker never reports state for the live one:
    // engine.py's superseded guard. Its TEXT events still arrive — the
    // audio was real speech.
    auto report_state = [&](State while_active) {
        if (session_generation_.load(std::memory_order_acquire) != generation) return;
        set_state(active_.load(std::memory_order_acquire) ? while_active : State::Idle);
    };

    while (auto first = session->queue.pop()) {
        // Merge with whatever else is ALREADY queued, so nothing waits
        // for audio that has not happened yet (engine.py's
        // _drain_queued): with a free worker the first utterance goes
        // alone and immediately; batching appears only when the user talks
        // faster than the machine transcribes. Adjacent audio also gives
        // the model more context than a two-word fragment has — the text
        // reads as a sentence rather than scraps.
        merged = std::move(first->samples);
        while (merged.size() < kMergeLimitSamples) {
            auto next = session->queue.try_pop();
            if (!next) break;
            merged.insert(merged.end(), next->samples.begin(), next->samples.end());
        }

        report_state(State::Transcribing);

        // The energy gates decided this was loud enough; Silero decides
        // whether it is actually a voice — where fan noise and keyboard
        // clacks are rejected before Whisper can hallucinate subtitle
        // credits out of them. Fail-open: a VAD that cannot run must not
        // silently swallow utterances.
        if (cfg.vad && !vad_->contains_speech(merged.data(), merged.size())) {
            report_state(State::Listening);
        } else {
            auto text = stt_->transcribe(merged.data(), merged.size(), cfg.language,
                                         cfg.prompt.empty() ? constants::default_russian_prompt
                                                            : cfg.prompt);
            if (!text) {
                // The utterance is lost either way — vanishing silently
                // leaves the user speaking into a session that types
                // nothing (W2-M6). Surface it; the worker stays alive.
                emit_problem("transcription_failed",
                             "Transcription failed, utterance dropped.");
                report_state(State::Listening);
            } else if (!text->empty() && !filter::is_hallucination(*text)) {
                if (events_.text) events_.text(spaced(*text, typed_in_session));
                typed_in_session = true;
                report_state(State::Listening);
            } else {
                // Empty decode, or a hallucination the filter owns. Real
                // dictation never lands here; nothing is typed.
                report_state(State::Listening);
            }
        }

        if (session->queue.closed_and_empty()) break;
    }
}

// --- idle unload ---------------------------------------------------------------------

void Engine::schedule_idle_unload() {
    // Never called with mutex_/events_mutex_ held. Only the newest
    // teardown's timer counts.
    double timeout;
    int generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++idle_generation_;
        generation = idle_generation_.load();
        timeout = config_.idle_timeout;
        if (quitting_.load(std::memory_order_acquire)) return;
    }
    {
        std::lock_guard<std::mutex> wake(idle_mutex_);
        idle_cv_.notify_all(); // wake any previous timer; it sees the bump and exits
    }
    if (timeout <= 0) return; // 0 = never unload; back-to-back stays instant

    std::lock_guard<std::mutex> lock(mutex_);
    auto owned = std::make_shared<OwnedThread>();
    owned->thread = std::jthread([this, generation, timeout] {
        std::unique_lock<std::mutex> wait_lock(idle_mutex_);
        bool cancelled = idle_cv_.wait_for(wait_lock,
                                            std::chrono::duration<double>(timeout),
                                            [this, generation] {
                                                return quitting_.load(std::memory_order_acquire) ||
                                                       idle_generation_.load() != generation;
                                            });
        wait_lock.unlock();
        if (cancelled) return;
        // Re-check under the lifecycle lock: a start that raced the timer
        // sets start_pending_/active_ before its loader can touch the
        // model, so this cannot unload a model someone is about to use.
        std::lock_guard<std::mutex> lock(mutex_);
        if (quitting_.load(std::memory_order_acquire) ||
            active_.load(std::memory_order_acquire) ||
            start_pending_.load(std::memory_order_acquire))
            return;
        if (stt_->is_loaded()) {
            stt_->unload();
            // The VAD goes with it: "costs nothing at all once idle" means
            // no model of ours left resident, and the next session reloads
            // both on the same loader thread.
            vad_->unload();
            if (events_.model) events_.model(ModelStatus::Unloaded, "");
        }
    });
    threads_.push_back(std::move(owned));
}

// --- the spacing rule ------------------------------------------------------------------

std::string Engine::spaced(const std::string& text, bool typed_in_session) {
    // A separator between one utterance and the next, within a session:
    // Whisper returns each utterance trimmed, so typing them as they come
    // runs them together — "the fee is computedon gross". A leading space
    // fixes that, except before punctuation that belongs to the word before
    // it, and except the first utterance of a session.
    if (!typed_in_session || text.empty()) return text;
    if (starts_with_leading_punct(text)) return text;
    return " " + text;
}

} // namespace mynah::session