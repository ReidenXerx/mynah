// Fake STT and VAD for the session and fuzz suites: no model files, no
// whisper, fully scriptable. The real whisper path is exercised end to end
// by mynah-replay.

#pragma once

#include <atomic>
#include <functional>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "stt/stt.hpp"
#include "vad/vad.hpp"

namespace mynah_test {

class FakeStt final : public mynah::stt::SpeechToText {
public:
    std::atomic<bool> loaded{false};
    bool load_result = true;
    // Runs inside load(); a test can block a cold load and race stop()
    // against it.
    std::function<void()> on_load;

    std::mutex mutex;
    struct Call {
        std::size_t sample_count = 0;
        std::string language;
        std::string prompt;
    };
    std::vector<Call> calls;
    std::vector<std::size_t> call_sample_counts; // mirror of calls
    std::function<std::string()> result = [] { return "привет мир"; };

    bool load(const std::filesystem::path&) override {
        if (on_load) on_load();
        loaded.store(true);
        return load_result;
    }
    bool is_loaded() const override { return loaded.load(); }
    void unload() override { loaded.store(false); }
    std::optional<std::string> transcribe(const float*, std::size_t count,
                                          const std::string& language,
                                          const std::string& prompt) override {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(Call{count, language, prompt});
        call_sample_counts.push_back(count);
        return result();
    }
};

class FakeVad final : public mynah::vad::VoiceActivity {
public:
    std::atomic<bool> loaded{false};
    std::atomic<bool> speech{true};
    bool load_result = true;

    bool load(const std::filesystem::path&) override {
        loaded.store(true);
        return load_result;
    }
    bool is_loaded() const override { return loaded.load(); }
    void unload() override { loaded.store(false); }
    bool contains_speech(const float*, std::size_t) override { return speech.load(); }
};

} // namespace mynah_test