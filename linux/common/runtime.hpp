// mynah::runtime — the running engine, as every Linux front end runs it.
//
// The headless `mynah` and `mynah-kde` run the same thing: libmynah, typing
// (make_auto: KWin's paste typer on Plasma, wtype elsewhere) behind the
// TypingQueue, the control socket (so `mynah toggle`, `mynah watch` and the
// Omarchy plugin work whichever front end owns the engine), and PipeWire
// capture. What differs is only who listens — a terminal, or a tray and a
// pill — so that is the Listener.
//
// The ordering rules live here once (they were learned the hard way):
//   - typing and the socket are wired into the event context BEFORE the
//     socket accepts a command, the only thing that can start a session;
//   - on stop the engine goes first (no event after it), then what is
//     still queued is typed while subscribers can still hear about it,
//     then the socket.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mynah/mynah.h"

namespace mynah::control {
class Server;
}
namespace mynah::inject {
class Injector;
class TypingQueue;
} // namespace mynah::inject
namespace mynah::capture {
class PipeWireCapture;
}

namespace mynah::runtime {

// "idle", "loading", "listening", "transcribing" — the protocol's words.
const char* state_word(mynah_state state);

// What a front end hears. Every callback runs on an engine or typing
// thread and must not block: marshal to your own thread. Any may be empty.
struct Listener {
    std::function<void(mynah_state)> state;
    std::function<void(float level, const float* bands)> level; // bands: 12 values
    std::function<void(const std::string& text)> typed;         // after it landed
    std::function<void(const std::string& code, const std::string& message)> problem;
    std::function<void(mynah_model_status status, const std::string& name)> model;
    std::function<void()> quit; // `mynah quit` arrived on the socket
};

class Runtime {
public:
    explicit Runtime(Listener listener = {});
    ~Runtime(); // stop()

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Engine, typing, socket, microphone, in that order. false with `error`
    // set — "the engine could not start: …", the socket's own message
    // (another mynah owns it), or "microphone: …" — and nothing left running.
    bool start(std::string& error);
    // Idempotent; see the ordering above.
    void stop();

    mynah_engine* engine() const { return engine_; }
    std::string socket_path() const;
    // The typing route's own verdict (Injector::check): (ok, remedy).
    std::pair<bool, std::string> typing_check() const;
    // Where speech runs: the GPU's name, or "the CPU" (set by start()).
    const std::string& speech_device() const { return speech_device_; }

private:
    static void on_event(const mynah_event* event, void* user);

    Listener listener_;
    mynah_engine* engine_ = nullptr;
    std::unique_ptr<inject::Injector> injector_;
    std::unique_ptr<control::Server> server_;
    std::unique_ptr<inject::TypingQueue> typing_;
    std::unique_ptr<capture::PipeWireCapture> capture_;
    std::string speech_device_;
};

} // namespace mynah::runtime
