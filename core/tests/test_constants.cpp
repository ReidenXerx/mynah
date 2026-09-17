// Pins the core's compiled-in constants against tuning/tuning.toml — the
// shared contract. The file is deliberately NOT read at runtime by any
// engine; it exists so that a value changed in one implementation without
// the others fails a test somewhere. This is the C++ twin of
// tests/test_tuning.py (Python) and macos/Tests/MynahAppTests/TuningTests.swift
// (Swift); when a pin here changes, change its twins too.

#include "vendor/doctest.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <string>

#include "config/config.hpp"
#include "config/flat_toml.hpp"
#include "filter/transcript_filter.hpp"
#include "mynah/mynah.h"
#include "tuning/constants.hpp"

namespace {

using mynah::flat_toml::Table;
using mynah::flat_toml::Value;

Table load_tuning() {
    std::ifstream in(MYNAH_TUNING_TOML, std::ios::binary);
    REQUIRE_MESSAGE(in, "cannot open " MYNAH_TUNING_TOML
                         " — is the tests binary run from a checkout?");
    std::string text((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    return mynah::flat_toml::parse(text);
}

// Numeric value for a key, accepting int-or-double the way from_values does
// (Python writes both shapes for the same field).
double number(const Table& tuning, const char* key) {
    auto it = tuning.find(key);
    // doctest's *_MESSAGE macros expand to `mb * <expr>`, so a multi-term
    // concatenation needs its own parentheses or * binds first.
    REQUIRE_MESSAGE(it != tuning.end(),
                    (std::string("tuning.toml: key '") + key + "' is missing"));
    if (const auto* d = std::get_if<double>(&it->second)) return *d;
    if (const auto* i = std::get_if<std::int64_t>(&it->second))
        return static_cast<double>(*i);
    FAIL((std::string("tuning.toml: key '") + key + "' is not numeric"));
    return 0.0;
}

std::vector<std::string> string_array(const Table& tuning, const char* key) {
    auto it = tuning.find(key);
    REQUIRE_MESSAGE(it != tuning.end(),
                    (std::string("tuning.toml: key '") + key + "' is missing"));
    const auto* items = std::get_if<std::vector<std::string>>(&it->second);
    REQUIRE_MESSAGE(items != nullptr,
                    (std::string("tuning.toml: key '") + key +
                     "' is not a string array — if this fails, the parser "
                     "lost the multi-line array"));
    return *items;
}

} // namespace

TEST_CASE("segmentation and calibration constants match tuning.toml") {
    Table tuning = load_tuning();
    CHECK(mynah::constants::utterance_silence == number(tuning, "utterance_silence"));
    CHECK(mynah::constants::trailing_padding == number(tuning, "trailing_padding"));
    CHECK(mynah::constants::noise_calibration_seconds ==
          number(tuning, "noise_calibration_seconds"));
    CHECK(mynah::constants::noise_frame_multiplier ==
          number(tuning, "noise_frame_multiplier"));
    CHECK(mynah::constants::noise_utterance_multiplier ==
          number(tuning, "noise_utterance_multiplier"));
    CHECK(mynah::constants::noise_min_samples ==
          static_cast<int>(number(tuning, "noise_min_samples")));
    CHECK(mynah::constants::calibration_speech_floor ==
          number(tuning, "calibration_speech_floor"));
}

TEST_CASE("Whisper decoder thresholds match tuning.toml (W2-M12)") {
    Table tuning = load_tuning();
    // Compare in FLOAT space, exactly as TuningTests.swift does: the core's
    // constants are float (whisper.cpp's API), and Float(0.35) widened to
    // double is 0.3499999940…, which is not the double literal 0.35 — an
    // exact == in double space can never pass even when both sides mean the
    // same value.
    CHECK(mynah::constants::whisper_no_speech_threshold ==
          static_cast<float>(number(tuning, "whisper_no_speech_threshold")));
    CHECK(mynah::constants::whisper_logprob_threshold ==
          static_cast<float>(number(tuning, "whisper_logprob_threshold")));
}

TEST_CASE("Config defaults match tuning.toml") {
    Table tuning = load_tuning();
    mynah::config::Config config;
    CHECK(config.frame_energy == number(tuning, "frame_energy_default"));
    CHECK(config.min_energy == number(tuning, "min_energy_default"));
    CHECK(config.min_utterance == number(tuning, "min_utterance_default"));
}

TEST_CASE("hallucination phrases match tuning.toml (set equality, both lists)") {
    Table tuning = load_tuning();
    std::vector<std::string> artifacts = string_array(tuning, "hallucination_artifact_phrases");
    std::vector<std::string> vocab = string_array(tuning, "hallucination_vocab_phrases");

    std::set<std::string> artifact_set(artifacts.begin(), artifacts.end());
    std::set<std::string> vocab_set(vocab.begin(), vocab.end());
    std::set<std::string> impl_artifacts(mynah::filter::artifact_phrases.begin(),
                                         mynah::filter::artifact_phrases.end());
    std::set<std::string> impl_vocab(mynah::filter::vocab_phrases.begin(),
                                     mynah::filter::vocab_phrases.end());

    CHECK(impl_artifacts == artifact_set);
    CHECK(impl_vocab == vocab_set);
    // Set equality would hide a duplicated entry in the file; count pins it.
    CHECK(artifacts.size() == mynah::filter::artifact_phrases.size());
    CHECK(vocab.size() == mynah::filter::vocab_phrases.size());
    // The two modes must not share a phrase: a vocab word listed as an
    // artifact would be substring-matched — exactly the CRITICAL the split
    // exists to prevent.
    for (const auto& phrase : vocab_set)
        CHECK_MESSAGE(impl_artifacts.count(phrase) == 0,
                      ("vocab phrase is also an artifact: " + phrase));
}

TEST_CASE("tuning.toml parses with exactly the contract keys") {
    Table tuning = load_tuning();
    std::set<std::string> expected{
        "utterance_silence",
        "trailing_padding",
        "noise_calibration_seconds",
        "noise_frame_multiplier",
        "noise_utterance_multiplier",
        "noise_min_samples",
        "calibration_speech_floor",
        "frame_energy_default",
        "min_energy_default",
        "min_utterance_default",
        "hallucination_artifact_phrases",
        "hallucination_vocab_phrases",
        "whisper_no_speech_threshold",
        "whisper_logprob_threshold",
    };
    std::set<std::string> actual;
    for (const auto& [key, value] : tuning) actual.insert(key);
    CHECK(actual == expected);
}

TEST_CASE("the C API's band count is the engine's band count") {
    // The event contract (MYNAH_SPECTRUM_BANDS in mynah.h) and the engine's
    // spectrum (P4) must not drift apart — front ends size their arrays from
    // the header.
    CHECK(mynah::constants::spectrum_bands == MYNAH_SPECTRUM_BANDS);
}