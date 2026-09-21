#include "pipewire_capture.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/result.h>

#include <cstring>

#include "mynah/mynah.h"

namespace mynah::capture {

struct PipeWireCapture::Impl {
    mynah_engine* engine = nullptr;
    std::string target;
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};
    bool connected = false;
};

namespace {

void on_process(void* data) {
    auto* impl = static_cast<PipeWireCapture::Impl*>(data);
    pw_buffer* buffer = pw_stream_dequeue_buffer(impl->stream);
    if (buffer == nullptr) return;
    spa_data& spa = buffer->buffer->datas[0];
    // F32_LE frames: the bytes are our float samples already.
    if (spa.data != nullptr && spa.chunk->size > 0) {
        const float* samples = reinterpret_cast<const float*>(spa.data);
        std::size_t count = std::size_t(spa.chunk->size) / sizeof(float);
        // The real-time path: into the ring, never blocking.
        mynah_push_audio(impl->engine, samples, count);
    }
    pw_stream_queue_buffer(impl->stream, buffer);
}

// By hand rather than designated initializers: the struct's field set moves
// between PipeWire releases, and a positional init against a different
// header is a silent mis-wire.
pw_stream_events stream_events() {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.process = on_process;
    return events;
}

} // namespace

PipeWireCapture::PipeWireCapture(mynah_engine* engine, std::string target)
    : impl_(new Impl), error_() {
    impl_->engine = engine;
    impl_->target = std::move(target);
}

PipeWireCapture::~PipeWireCapture() {
    stop();
    delete impl_;
}

bool PipeWireCapture::start() {
    pw_init(nullptr, nullptr);

    impl_->loop = pw_thread_loop_new("mynah-capture", nullptr);
    if (impl_->loop == nullptr) {
        error_ = "cannot create the PipeWire loop";
        return false;
    }
    impl_->context = pw_context_new(pw_thread_loop_get_loop(impl_->loop), nullptr, 0);
    if (impl_->context == nullptr) {
        error_ = "cannot create the PipeWire context";
        return false;
    }
    pw_thread_loop_lock(impl_->loop);
    impl_->core = pw_context_connect(impl_->context, nullptr, 0);
    if (impl_->core == nullptr) {
        pw_thread_loop_unlock(impl_->loop);
        error_ = "cannot connect to PipeWire — is the session's pipewire running?";
        return false;
    }

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Accessibility",  // a dictation tool's honest role
        nullptr);
    if (!impl_->target.empty())
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, impl_->target.c_str());

    impl_->stream = pw_stream_new(impl_->core, "mynah", props);
    if (impl_->stream == nullptr) {
        pw_thread_loop_unlock(impl_->loop);
        error_ = "cannot create the capture stream";
        return false;
    }
    pw_stream_events events = stream_events();
    pw_stream_add_listener(impl_->stream, &impl_->stream_listener, &events, impl_);

    // 16 kHz mono float32 — the engine's domain; the graph resamples.
    std::uint8_t pod_buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32_LE;
    info.rate = 16000;
    info.channels = 1;
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    const spa_pod* params[1] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &info),
    };

    int connected = pw_stream_connect(
        impl_->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (connected < 0) {
        pw_thread_loop_unlock(impl_->loop);
        error_ = "cannot connect the capture stream: " + std::string(spa_strerror(connected));
        return false;
    }
    pw_thread_loop_unlock(impl_->loop);

    pw_thread_loop_start(impl_->loop);
    impl_->connected = true;
    return true;
}

void PipeWireCapture::stop() {
    if (impl_ == nullptr || !impl_->connected) return;
    pw_thread_loop_lock(impl_->loop);
    if (impl_->stream != nullptr) {
        pw_stream_disconnect(impl_->stream);
        spa_hook_remove(&impl_->stream_listener);
        pw_stream_destroy(impl_->stream);
        impl_->stream = nullptr;
    }
    if (impl_->core != nullptr) {
        pw_core_disconnect(impl_->core);
        impl_->core = nullptr;
    }
    if (impl_->context != nullptr) {
        pw_context_destroy(impl_->context);
        impl_->context = nullptr;
    }
    pw_thread_loop_unlock(impl_->loop);
    pw_thread_loop_stop(impl_->loop);
    pw_thread_loop_destroy(impl_->loop);
    impl_->loop = nullptr;
    impl_->connected = false;
}

} // namespace mynah::capture