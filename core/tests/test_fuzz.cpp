// Short fuzz runs of the two attack surfaces a front end can poke at
// blindly (the Phase 2 exit criterion): mynah_push_audio — arbitrary sizes,
// arbitrary sample values, at arbitrary times — and the config parser,
// fed byte mutations of real files. Deterministic seeds, so a failure is
// reproducible; the same seeds under ASan/TSan catch what a plain run
// cannot see.
//
// This is a smoke fuzzer, not a coverage fuzzer: it exists so the suite
// dies loudly on a bad free or an infinite loop without needing libFuzzer
// in the build.

#include "vendor/doctest.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "config/flat_toml.hpp"
#include "env.hpp"
#include "fakes.hpp"
#include "json.hpp"
#include "session/session.hpp"

namespace {

// xorshift64* — deterministic, fast, and seeded per iteration.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ull;
    }
    std::size_t below(std::size_t bound) { return std::size_t(next() % bound); }
};

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

mynah::config::Config fuzz_config() {
    mynah::config::Config config;
    config.idle_timeout = 0;
    config.auto_stop_silence = 0;
    config.vad = false;
    return config;
}

} // namespace

TEST_CASE("push_audio fuzz: random sizes and values never hang or corrupt the engine") {
    // Several short runs rather than one long one: each exercises a full
    // session lifecycle (start, a burst of pushes, stop, destroy), which
    // is where races would bite.
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        CAPTURE(seed);
        Rng rng(seed * 0xD1B54A32D192ED03ull);

        auto stt = std::make_unique<mynah_test::FakeStt>();
        auto vad = std::make_unique<mynah_test::FakeVad>();
        mynah::session::Engine engine(fuzz_config(), std::move(stt), std::move(vad),
                                     mynah::session::Events{});

        engine.start();
        std::vector<float> buffer(4096);
        for (int i = 0; i < 6000; ++i) {
            std::size_t count = rng.below(buffer.size() + 1); // 0..4096, empty allowed
            std::size_t pattern = rng.below(4);
            for (std::size_t j = 0; j < count; ++j) {
                switch (pattern) {
                case 0: buffer[j] = float(rng.next()) / float(UINT64_MAX); break;
                case 1: buffer[j] = float(rng.next() & 0xFF) / 128.0f - 1.0f; break;
                case 2:
                    buffer[j] = (rng.next() & 1) ? std::numeric_limits<float>::quiet_NaN()
                                                 : std::numeric_limits<float>::infinity();
                    break; // garbage floats: the engine must not care
                default: buffer[j] = 0.0f; break;
                }
            }
            engine.push_audio(buffer.data(), count);
            if (rng.below(1000) == 0) engine.stop(); // rare mid-burst stop
            if (rng.below(1000) == 0) engine.start(); // and restart
        }
        engine.stop();
        // Destroy joins everything: reaching here means no hang, no crash.
    }
}

TEST_CASE("config parser fuzz: byte mutations never crash, and emit round-trips stay stable") {
    std::vector<std::string> corpus;
    corpus.push_back(read_file(MYNAH_TUNING_TOML));
    corpus.push_back(R"(
# a hand-written config with everything the writers emit
language = "ru"
prompt = "многострочная \"кавычки\" и \\ слэш"
idle_timeout = 45.0
hotkey = "<cmd>+<shift>+."
trigger = "toggle"
vad = true
auto_stop_silence = 10.0
show_indicator = true
idle_visible = false
frame_energy = 0.010 # comment
min_energy = 0.008
min_utterance = 0.25
model_dirs = ["/one", "/a,b", "/c#d"]
empty = []
)");
    corpus.push_back(read_file(MYNAH_TUNING_TOML)); // mutations of the big one twice
    REQUIRE(!corpus[0].empty());

    for (std::uint64_t seed = 1; seed <= 30; ++seed) {
        Rng rng(seed * 0x2545F4914F6CDD1Dull);
        for (int i = 0; i < 400; ++i) {
            std::string text = corpus[rng.below(corpus.size())];
            for (int mutation = 0; mutation < 8; ++mutation) {
                if (text.empty()) break;
                switch (rng.below(3)) {
                case 0: { // flip a byte
                    std::size_t at = rng.below(text.size());
                    text[at] = char(std::uint8_t(text[at]) ^ std::uint8_t(1u << rng.below(8)));
                    break;
                }
                case 1: { // truncate
                    text.resize(rng.below(text.size()));
                    break;
                }
                default: { // insert a byte
                    std::size_t at = rng.below(text.size() + 1);
                    text.insert(at, 1, char(std::uint8_t(rng.next())));
                    break;
                }
                }
            }

            // Parse never crashes (it may drop keys — that is the documented
            // lossy behaviour); emit/parse round-trips stably.
            mynah::flat_toml::Table first = mynah::flat_toml::parse(text);
            std::string emitted = mynah::flat_toml::emit(first);
            mynah::flat_toml::Table second = mynah::flat_toml::parse(emitted);
            mynah::flat_toml::Table third = mynah::flat_toml::parse(mynah::flat_toml::emit(second));
            CHECK_MESSAGE(second == first, "emit/parse is not a stable round trip");
            CHECK_MESSAGE(third == second, "emit/parse is not idempotent");
        }
    }
}

TEST_CASE("json parser fuzz: mutations of expected.json throw cleanly or parse") {
    std::string corpus = read_file(MYNAH_TUNING_TOML); // unrelated bytes: must not parse
    std::string golden = R"({"case": [{"start": 1.2, "end": 3.18,
        "rejected_by_energy_gate": false, "rejected_by_min_utterance": false}]})";
    REQUIRE(mynah::json::parse(golden).is_object());
    bool throws_on_garbage = false;
    try {
        (void)mynah::json::parse(corpus);
    } catch (const std::exception&) {
        throws_on_garbage = true;
    }
    CHECK(throws_on_garbage);

    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Rng rng(seed * 0x9E3779B97F4A7C15ull);
        for (int i = 0; i < 200; ++i) {
            std::string text = golden;
            for (int mutation = 0; mutation < 4 && !text.empty(); ++mutation) {
                std::size_t at = rng.below(text.size());
                switch (rng.below(3)) {
                case 0: text[at] = char(std::uint8_t(text[at]) ^ std::uint8_t(1 << rng.below(8))); break;
                case 1: text.resize(rng.below(text.size())); break;
                default: text.insert(at, 1, char(std::uint8_t(rng.next()))); break;
                }
            }
            try {
                (void)mynah::json::parse(text); // must parse or throw, never crash
            } catch (const std::exception&) {
            }
        }
    }
}