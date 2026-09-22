#include "pipewire_capture.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mynah/mynah.h"

namespace mynah::capture {

struct PipeWireCapture::Impl {
    mynah_engine* engine = nullptr;
    std::string target;
    bool initialized = false; // pw_init ran, so pw_deinit is owed
    pw_thread_loop* loop = nullptr;
    bool loop_running = false;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};
    // Set on the loop thread when the format is negotiated, read on the
    // real-time thread: samples go to the engine only once they are known
    // to be 16 kHz mono float32 — anything else would be pushed as noise.
    std::atomic<bool> format_ok{false};
};

namespace {

void on_process(void* data) {
    auto* impl = static_cast<PipeWireCapture::Impl*>(data);
    pw_buffer* buffer = pw_stream_dequeue_buffer(impl->stream);
    if (buffer == nullptr) return;
    spa_data& spa = buffer->buffer->datas[0];
    if (spa.data != nullptr && spa.chunk != nullptr &&
        impl->format_ok.load(std::memory_order_acquire)) {
        // The valid bytes are [offset, offset + size) of a mapping of
        // maxsize bytes: honour both, and never read past the mapping.
        std::uint32_t offset = std::min(spa.chunk->offset, spa.maxsize);
        std::uint32_t size = std::min(spa.chunk->size, spa.maxsize - offset);
        std::size_t count = size / sizeof(float);
        if (count > 0) {
            // F32_LE mono: the bytes are our float samples already.
            const float* samples = SPA_PTROFF(spa.data, offset, const float);
            // The real-time path: into the ring, never blocking.
            mynah_push_audio(impl->engine, samples, count);
        }
    }
    pw_stream_queue_buffer(impl->stream, buffer);
}

void on_param_changed(void* data, std::uint32_t id, const spa_pod* param) {
    auto* impl = static_cast<PipeWireCapture::Impl*>(data);
    if (id != SPA_PARAM_Format) return;
    if (param == nullptr) { // format cleared: the stream is renegotiating
        impl->format_ok.store(false, std::memory_order_release);
        return;
    }
    spa_audio_info_raw info{};
    bool ok = spa_format_audio_raw_parse(param, &info) >= 0 &&
              info.format == SPA_AUDIO_FORMAT_F32_LE && info.rate == 16000 &&
              info.channels == 1;
    impl->format_ok.store(ok, std::memory_order_release);
    if (!ok)
        std::fprintf(stderr,
                     "mynah: microphone: PipeWire negotiated format %u, %u Hz, %u channels "
                     "instead of float32 16 kHz mono; ignoring its audio\n",
                     unsigned(info.format), unsigned(info.rate), unsigned(info.channels));
}

void on_state_changed(void*, pw_stream_state, pw_stream_state state, const char* error) {
    if (state == PW_STREAM_STATE_ERROR)
        std::fprintf(stderr, "mynah: microphone: the capture stream failed: %s\n",
                     error ? error : "unknown error");
}

// PipeWire keeps a pointer to the events table for as long as the listener
// is registered, so it must outlive every stream: static storage. Filled
// by hand rather than by designated initializers: the struct's field set
// moves between PipeWire releases, and a positional init against a
// different header is a silent mis-wire.
const pw_stream_events* stream_events() {
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = on_state_changed;
        e.param_changed = on_param_changed;
        e.process = on_process;
        return e;
    }();
    return &events;
}

// Undo whatever start() got as far as, in the order PipeWire wants: the
// stream and core go under the loop lock while its thread still runs, then
// the thread stops, and only then the context and loop go.
void teardown(PipeWireCapture::Impl& impl) {
    if (impl.loop != nullptr && impl.loop_running) pw_thread_loop_lock(impl.loop);
    if (impl.stream != nullptr) {
        pw_stream_disconnect(impl.stream);
        spa_hook_remove(&impl.stream_listener);
        pw_stream_destroy(impl.stream);
        impl.stream = nullptr;
    }
    if (impl.core != nullptr) {
        pw_core_disconnect(impl.core);
        impl.core = nullptr;
    }
    if (impl.loop != nullptr && impl.loop_running) {
        pw_thread_loop_unlock(impl.loop);
        pw_thread_loop_stop(impl.loop);
        impl.loop_running = false;
    }
    if (impl.context != nullptr) {
        pw_context_destroy(impl.context);
        impl.context = nullptr;
    }
    if (impl.loop != nullptr) {
        pw_thread_loop_destroy(impl.loop);
        impl.loop = nullptr;
    }
    if (impl.initialized) {
        pw_deinit();
        impl.initialized = false;
    }
    impl.format_ok.store(false, std::memory_order_release);
}

} // namespace

PipeWireCapture::PipeWireCapture(mynah_engine* engine, std::string target)
    : impl_(std::make_unique<Impl>()) {
    impl_->engine = engine;
    impl_->target = std::move(target);
}

PipeWireCapture::~PipeWireCapture() { stop(); }

bool PipeWireCapture::start() {
    if (impl_->stream != nullptr) return true; // already capturing
    auto failed = [this](std::string message) {
        teardown(*impl_);
        error_ = std::move(message);
        return false;
    };

    pw_init(nullptr, nullptr);
    impl_->initialized = true;

    impl_->loop = pw_thread_loop_new("mynah-capture", nullptr);
    if (impl_->loop == nullptr) return failed("cannot create the PipeWire loop");
    impl_->context = pw_context_new(pw_thread_loop_get_loop(impl_->loop), nullptr, 0);
    if (impl_->context == nullptr) return failed("cannot create the PipeWire context");
    if (pw_thread_loop_start(impl_->loop) < 0)
        return failed("cannot start the PipeWire loop thread");
    impl_->loop_running = true;

    pw_thread_loop_lock(impl_->loop);
    // Everything below runs under the lock; each failure unlocks first,
    // because teardown() takes the lock itself.
    auto failed_locked = [this, &failed](std::string message) {
        pw_thread_loop_unlock(impl_->loop);
        return failed(std::move(message));
    };

    impl_->core = pw_context_connect(impl_->context, nullptr, 0);
    if (impl_->core == nullptr)
        return failed_locked("cannot connect to PipeWire — is the session's pipewire running?");

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Accessibility",  // a dictation tool's honest role
        nullptr);
    if (!impl_->target.empty())
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, impl_->target.c_str());

    impl_->stream = pw_stream_new(impl_->core, "mynah", props); // takes props
    if (impl_->stream == nullptr) return failed_locked("cannot create the capture stream");
    pw_stream_add_listener(impl_->stream, &impl_->stream_listener, stream_events(),
                           impl_.get());

    // 16 kHz mono float32 — the engine's domain; the graph resamples. Offered
    // as the stream's only EnumFormat, so the adapter converts to it.
    std::uint8_t pod_buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32_LE;
    info.rate = 16000;
    info.channels = 1;
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    const spa_pod* params[1] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info),
    };

    int connected = pw_stream_connect(
        impl_->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (connected < 0)
        return failed_locked("cannot connect the capture stream: " +
                             std::string(spa_strerror(connected)));
    pw_thread_loop_unlock(impl_->loop);
    return true;
}

void PipeWireCapture::stop() {
    if (impl_) teardown(*impl_);
}

} // namespace mynah::capture
