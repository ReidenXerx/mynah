// mynah::capture — PipeWire capture, Linux's audio in (P7).
//
// The Python engine captured through PortAudio; the core goes straight to
// libpipewire: Omarchy and Plasma both run PipeWire, it negotiates the
// format we ask for, and there is no PortAudio layer to keep compatible.
// The stream requests 16 kHz mono float32 — the engine's own domain — so
// the graph resamples for us; the process callback runs on PipeWire's
// real-time thread and does nothing but mynah_push_audio, which is the
// contract that call was built for (never blocks, never allocates).

#pragma once

#include <memory>
#include <string>

struct mynah_engine;

namespace mynah::capture {

class PipeWireCapture {
public:
    // `engine` receives the samples; `name` is the target node to capture
    // from (empty = the default source).
    explicit PipeWireCapture(mynah_engine* engine, std::string target = {});
    ~PipeWireCapture();

    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    // Connect the stream. Returns false with `error()` set when PipeWire
    // is not reachable — the daemon not running is the normal reason on a
    // bare ssh session, and the remedy names it. A failed start leaves
    // nothing behind, and start() may be called again.
    bool start();
    // Idempotent; the destructor calls it.
    void stop();

    const std::string& error() const { return error_; }

    // Declared public but defined only in the .cpp, so it stays opaque:
    // PipeWire's C callbacks are free functions handed a void*, and they
    // have to be able to name the type they cast it back to.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

} // namespace mynah::capture