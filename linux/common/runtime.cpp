#include "runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "config/config.hpp"
#include "control_socket.hpp"
#include "injector.hpp"
#include "json.hpp"
#include "pipewire_capture.hpp"
#include "stt/stt.hpp"
#include "tuning/constants.hpp"
#include "typing_queue.hpp"

namespace mynah::runtime {

namespace {

std::string bands_json(const float* bands) {
    std::string json;
    for (int i = 0; i < constants::spectrum_bands; ++i) {
        char text[16];
        std::snprintf(text, sizeof(text), i ? ",%.3f" : "%.3f", double(bands[i]));
        json += text;
    }
    return json;
}

} // namespace

const char* state_word(mynah_state state) {
    switch (state) {
    case MYNAH_IDLE: return "idle";
    case MYNAH_LOADING: return "loading";
    case MYNAH_LISTENING: return "listening";
    case MYNAH_TRANSCRIBING: return "transcribing";
    }
    return "idle";
}

Runtime::Runtime(Listener listener) : listener_(std::move(listener)) {}

Runtime::~Runtime() { stop(); }

std::pair<bool, std::string> Runtime::typing_check() const {
    if (!injector_) return {false, "mynah is not running"};
    return injector_->check();
}

std::string Runtime::socket_path() const {
    return server_ ? server_->path() : control::socket_path();
}

void Runtime::on_event(const mynah_event* event, void* user) {
    auto* self = static_cast<Runtime*>(user);
    const Listener& listener = self->listener_;
    switch (mynah_event_get_kind(event)) {
    case MYNAH_EVENT_STATE: {
        const mynah_state state = mynah_event_state(event);
        self->server_->publish("{\"event\":\"state\",\"state\":\"" +
                               std::string(state_word(state)) + "\"}");
        if (listener.state) listener.state(state);
        break;
    }
    case MYNAH_EVENT_LEVEL: {
        const float* bands = mynah_event_bands(event);
        self->server_->publish_level(double(mynah_event_level(event)), bands_json(bands));
        if (listener.level) listener.level(mynah_event_level(event), bands);
        break;
    }
    case MYNAH_EVENT_TEXT:
        // Typing blocks and this callback must not (mynah.h): queued,
        // typed on the queue's thread, published there once it landed.
        if (const char* text = mynah_event_text(event)) self->typing_->push(text);
        break;
    case MYNAH_EVENT_PROBLEM: {
        const char* code = mynah_event_problem_code(event);
        const char* message = mynah_event_problem_message(event);
        self->server_->publish(
            "{\"event\":\"problem\",\"code\":" + json::quoted(code ? code : "") +
            ",\"message\":" + json::quoted(message ? message : "") + "}");
        if (listener.problem) listener.problem(code ? code : "", message ? message : "");
        break;
    }
    case MYNAH_EVENT_MODEL: {
        static const char* names[] = {"loading", "loaded", "unloaded"};
        const mynah_model_status status = mynah_event_model_status(event);
        const char* name = mynah_event_model_name(event);
        std::string json = std::string("{\"event\":\"model\",\"status\":\"") +
                           names[int(status)] + "\"";
        if (name) json += ",\"name\":" + json::quoted(name);
        json += "}";
        self->server_->publish(json);
        if (listener.model) listener.model(status, name ? name : "");
        break;
    }
    }
}

bool Runtime::start(std::string& error) {
    if (engine_) return true;
    const config::Config config = config::load();

    // Vulkan starts here, not on the first press: it takes ~2 s, more when
    // it wakes a dGPU from D3cold, and that used to be spent while the start
    // of the first sentence was being dropped. The dGPU goes back to sleep
    // on its own ten seconds later.
    if (std::optional<stt::GpuChoice> gpu = stt::pick_gpu(config.gpu))
        speech_device_ = gpu->name;
    else
        speech_device_ = "the CPU";

    // Typing exists before the engine's TEXT event can fire.
    const inject::Tools tools = inject::Tools::discover();
    if (config.injector == "wtype") injector_ = inject::make_wtype(tools);
    else if (config.injector == "clipboard") injector_ = inject::make_clipboard(tools);
    else injector_ = inject::make_auto(tools); // KWin's typer on KWin, smart elsewhere

    char* engine_error = nullptr;
    engine_ = mynah_create(nullptr, &Runtime::on_event, this, &engine_error);
    if (!engine_) {
        error = engine_error ? engine_error : "the engine could not start";
        std::free(engine_error);
        injector_.reset();
        return false;
    }

    // No event can fire until the socket below accepts a command, so both
    // are wired into the event path first.
    control::Server::Handlers wired;
    mynah_engine* engine = engine_;
    wired.toggle = [engine] { mynah_toggle(engine); };
    wired.start = [engine] { mynah_start(engine); };
    wired.stop = [engine] { mynah_stop(engine); };
    wired.quit = [this] {
        if (listener_.quit) listener_.quit();
    };
    wired.state = [engine] { return std::string(state_word(mynah_get_state(engine))); };
    server_ = std::make_unique<control::Server>(std::move(wired), control::socket_path(),
                                                mynah_version());
    // The `text` event means the text really landed: a failed injection
    // publishes nothing.
    typing_ = std::make_unique<inject::TypingQueue>(
        [this](const std::string& text) { return injector_->type_text(text); },
        [this](const std::string& text) {
            server_->publish("{\"event\":\"text\",\"text\":" + json::quoted(text) + "}");
            if (listener_.typed) listener_.typed(text);
        });
    try {
        server_->start();
    } catch (const std::exception& e) {
        error = e.what();
        stop();
        return false;
    }

    capture_ = std::make_unique<capture::PipeWireCapture>(engine_);
    if (!capture_->start()) {
        error = "microphone: " + capture_->error();
        stop();
        return false;
    }
    return true;
}

void Runtime::stop() {
    if (capture_) capture_->stop();
    if (engine_) {
        mynah_stop(engine_);
        mynah_destroy(engine_); // no event after this
        engine_ = nullptr;
    }
    typing_.reset(); // types what is queued; subscribers still hear it
    if (server_) server_->stop();
    capture_.reset();
    server_.reset();
    injector_.reset();
}

} // namespace mynah::runtime
