// The utterance detector against the shared golden corpus — the C++ twin
// of macos/Tests/MynahAppTests/TuningTests.swift and
// tests/test_segmentation_golden.py. A segmentation change that breaks
// cross-implementation agreement now fails on all three platforms.
//
// Region reconstruction is frame-space arithmetic, exactly as the Swift
// suite does it (NOT from int(silent_duration * rate) — the accumulated
// silence can be off by one sample in floating point, which would shift
// the start):
//   - end = start time of the frame that closed the region (the loop
//     index at emission), matching expected.json's `end`
//   - the buffer spans speechFrames + closeSilenceFrames frames; the
//     emitted samples are that buffer trimmed to trailing_padding of kept
//     silence, so speechFrames = (count - keep) / 480 by floor division,
//     robust to ±1 sample of trim.

#include "vendor/doctest.h"

#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "audio/analysis.hpp"
#include "golden.hpp"
#include "segment/utterance_detector.hpp"
#include "tuning/constants.hpp"

using mynah::segment::Utterance;
using mynah::segment::UtteranceDetector;
using mynah_test::ExpectedRegion;
using mynah_test::kGoldenCases;
using mynah_test::load_expected;
using mynah_test::load_wav;

namespace {

using namespace mynah; // constants::, audio:: in the helpers below

constexpr double kSampleRate = 16000.0;
constexpr int kFrameLen = 480;      // 30 ms — the contract's frame size
constexpr double kFrameSeconds = 0.03;

// Frames needed to close an utterance: smallest n with n*frame >= silence.
// 0.8 / 0.03 -> 27 — the same integer the detector reaches by accumulation.
int close_silence_frames() {
    int n = 1;
    while (n * kFrameSeconds < constants::utterance_silence) ++n;
    return n;
}

// A frame of constant samples has RMS == |amplitude| — the cheapest input
// with a known energy. The amplitudes in the calibration tests are dyadic
// (exact in Float and Double alike), so the median/gate arithmetic is
// exact too.
std::vector<float> constant_frame(float amplitude) {
    return std::vector<float>(kFrameLen, amplitude);
}

} // namespace

TEST_CASE("UtteranceDetector segments the golden corpus as pinned") {
    std::filesystem::path golden_dir = MYNAH_GOLDEN_DIR;
    auto expected = load_expected(golden_dir);
    constexpr double kMinUtterance = 0.25; // min_utterance_default, pinned by test_constants

    for (const char* case_name : kGoldenCases) {
        CAPTURE(std::string(case_name));
        std::vector<float> samples = load_wav(golden_dir, case_name);
        REQUIRE(expected.count(case_name) == 1);

        UtteranceDetector detector(kSampleRate, 0.010, 0.008);

        int keep = int(constants::trailing_padding * kSampleRate);
        struct Region {
            double start;
            double end;
            bool rejected_energy;
            bool rejected_min;
        };
        std::vector<Region> regions;
        int frame_index = 0;
        std::size_t i = 0;
        while (i + kFrameLen <= samples.size()) {
            if (auto utterance =
                    detector.process(samples.data() + i, kFrameLen)) {
                int speech_frames =
                    int(std::size_t(utterance->samples.size() < std::size_t(keep)
                                        ? 0
                                        : utterance->samples.size() - keep) / kFrameLen);
                int buffered_frames = speech_frames + close_silence_frames();
                int start_idx = frame_index - buffered_frames + 1;
                // The gates exactly as the session's enqueue applies them:
                // RMS against the calibrated gate, min_utterance against
                // the trimmed (P2) duration.
                bool rejected_energy = audio::rms(utterance->samples.data(),
                                                  utterance->samples.size()) <
                                       detector.current_energy_threshold();
                bool rejected_min = utterance->duration < kMinUtterance;
                regions.push_back(Region{double(start_idx) * kFrameSeconds,
                                        double(frame_index) * kFrameSeconds,
                                        rejected_energy, rejected_min});
            }
            ++frame_index;
            i += kFrameLen;
        }

        // No fixture ends mid-speech: nothing may remain buffered.
        REQUIRE(detector.flush() == std::nullopt);

        const std::vector<ExpectedRegion>& want = expected[case_name];
        REQUIRE_MESSAGE(regions.size() == want.size(),
                        (std::string(case_name) + ": produced " +
                         std::to_string(regions.size()) + " region(s), expected " +
                         std::to_string(want.size()))
                            .c_str());
        for (std::size_t r = 0; r < want.size(); ++r) {
            CHECK(std::abs(regions[r].start - want[r].start) < 1e-9);
            CHECK(std::abs(regions[r].end - want[r].end) < 1e-9);
            CHECK_MESSAGE(regions[r].rejected_energy == want[r].rejected_by_energy_gate,
                          (std::string(case_name) + ": energy-gate verdict " +
                           (regions[r].rejected_energy ? "true" : "false") +
                           " != expected " +
                           (want[r].rejected_by_energy_gate ? "true" : "false"))
                              .c_str());
            CHECK_MESSAGE(regions[r].rejected_min == want[r].rejected_by_min_utterance,
                          (std::string(case_name) + ": min-utterance verdict " +
                           (regions[r].rejected_min ? "true" : "false") +
                           " != expected " +
                           (want[r].rejected_by_min_utterance ? "true" : "false"))
                              .c_str());
        }
    }
}

TEST_CASE("expected.json holds exactly the pinned cases") {
    // Orphan guard: a case dropped from the list would silently stop being
    // tested, and a stray expected.json entry would never be noticed.
    auto expected = load_expected(MYNAH_GOLDEN_DIR);
    REQUIRE(expected.size() == std::size(kGoldenCases));
    for (const char* case_name : kGoldenCases)
        CHECK(expected.count(case_name) == 1);
}

TEST_CASE("speech in the calibration window aborts to the static gates — the first word survives") {
    // speech_during_calibration pins the speech-aware calibration fix:
    // speech fills the 1 s window, the speech frames are excluded from the
    // noise median, too few quiet frames remain, and calibration aborts to
    // the static gates. The region is segmented AND passes the energy
    // gate. Before the fix the median was measured on speech, the
    // utterance gate rose to ~3x the speech RMS, and the first word was
    // silently rejected.
    std::vector<float> samples = load_wav(MYNAH_GOLDEN_DIR, "speech_during_calibration");
    UtteranceDetector detector(kSampleRate, 0.010, 0.008);

    std::optional<Utterance> emitted;
    std::size_t i = 0;
    while (i + kFrameLen <= samples.size()) {
        if (auto utterance = detector.process(samples.data() + i, kFrameLen))
            emitted = std::move(utterance);
        i += kFrameLen;
    }

    REQUIRE(emitted.has_value());
    // A window full of speech must abort calibration: the gates stay at
    // the static floors — the median may never be measured on speech.
    CHECK(detector.current_energy_threshold() == 0.008);
    double rms = audio::rms(emitted->samples.data(), emitted->samples.size());
    CHECK_MESSAGE(rms >= detector.current_energy_threshold(),
                  "buffer RMS should pass the static gate — the first word must survive");
}

TEST_CASE("speech over noise excludes speech from the median but still adapts") {
    // speech_over_noise_in_calibration: fan-level noise then speech inside
    // the window. The speech frames are excluded from the median, the
    // noise frames raise the gates, and the utterance still clears the
    // raised gate. A regression to "any speech -> skip calibration" would
    // leave the gates static and let the noise tail pass — this pins the
    // exclusion mechanism itself.
    std::vector<float> samples =
        load_wav(MYNAH_GOLDEN_DIR, "speech_over_noise_in_calibration");
    UtteranceDetector detector(kSampleRate, 0.010, 0.008);

    std::optional<Utterance> emitted;
    std::size_t i = 0;
    while (i + kFrameLen <= samples.size()) {
        if (auto utterance = detector.process(samples.data() + i, kFrameLen))
            emitted = std::move(utterance);
        i += kFrameLen;
    }

    REQUIRE(emitted.has_value());
    // The quiet (noise) frames raise the gate — speech exclusion must not
    // disable adaptation.
    CHECK(detector.current_energy_threshold() > 0.008);
    double rms = audio::rms(emitted->samples.data(), emitted->samples.size());
    CHECK_MESSAGE(rms >= detector.current_energy_threshold(),
                  "buffer RMS should pass the raised gate");
}

// --- calibration median + abort boundary (the speech-aware block in
// tests/test_dictate.py, transplanted) ------------------------------------

TEST_CASE("even count of non-uniform quiet frames: the two middle values are averaged") {
    // Swift once took the upper-middle element for even counts while
    // engine.py averages the two middle values — a divergence the uniform
    // fixtures cannot see. 10 frames at 1/1024 + 10 at 1/128: the two
    // middle values differ, and the median must be their average
    // (9/2048), not the upper-middle element (1/128).
    UtteranceDetector detector(kSampleRate, 0.010, 0.008);

    // 34 frames x 0.03s covers the 1 s calibration window — 34 is
    // int(1.0 / 0.03) + 1, the frame count that triggers calibration.
    // 20 non-uniform quiet frames, then 14 speech frames
    // (0.0625 >= calibration_speech_floor — excluded).
    for (int i = 0; i < 20; ++i)
        detector.process(constant_frame(i % 2 == 0 ? 0.0009765625f : 0.0078125f).data(),
                          kFrameLen);
    for (int i = 20; i < 34; ++i)
        detector.process(constant_frame(0.0625f).data(), kFrameLen);
    // Median = (1/1024 + 1/128) / 2 = 9/2048; utterance gate = 9/2048 * 3.
    CHECK(detector.current_energy_threshold() == (0.0009765625 + 0.0078125) / 2 * 3.0);
}

TEST_CASE("exactly noiseMinimumSamples quiet frames still calibrates") {
    // The abort boundary is >= noise_min_samples: the minimum count of
    // quiet frames must still adapt. The quiet amplitude (1/128) keeps the
    // raised gate under the calibration cap so this pins the multiplier
    // arithmetic, not the cap.
    UtteranceDetector detector(kSampleRate, 0.010, 0.008);

    for (int i = 0; i < 5; ++i)
        detector.process(constant_frame(0.0078125f).data(), kFrameLen); // quiet
    for (int i = 5; i < 34; ++i)
        detector.process(constant_frame(0.0625f).data(), kFrameLen); // speech
    CHECK_MESSAGE(detector.current_energy_threshold() == 0.0078125 * 3.0,
                  "5 quiet frames is exactly noiseMinimumSamples — must adapt");
}

TEST_CASE("one quiet frame short of noiseMinimumSamples aborts") {
    // The boundary counterpart: 4 quiet frames is too few to trust; the
    // static gates stay in force. Together with the 5-quiet test this pins
    // the comparison as >= (not >).
    UtteranceDetector detector(kSampleRate, 0.010, 0.008);

    for (int i = 0; i < 4; ++i)
        detector.process(constant_frame(0.015625f).data(), kFrameLen); // quiet
    for (int i = 4; i < 34; ++i)
        detector.process(constant_frame(0.0625f).data(), kFrameLen); // speech
    CHECK_MESSAGE(detector.current_energy_threshold() == 0.008,
                  "4 quiet frames < noiseMinimumSamples — calibration must abort");
}

// --- the calibration cap (M13, wave-2) ---------------------------------------

TEST_CASE("a noisy median's gate contribution caps at the speech floor") {
    // A measured fan-level median (1/64 = 0.015625, the level recorded for
    // a MacBook under load) would raise the utterance gate to 3/64 ~= 0.047
    // — above the speech floor, i.e. a gate demanding speech louder than
    // speech, which no static floor setting could counter.
    UtteranceDetector detector(kSampleRate, 0.010, 0.008);

    for (int i = 0; i < 20; ++i)
        detector.process(constant_frame(0.015625f).data(), kFrameLen); // fan level
    for (int i = 20; i < 34; ++i)
        detector.process(constant_frame(0.0625f).data(), kFrameLen); // speech — excluded
    CHECK_MESSAGE(detector.current_energy_threshold() ==
                      constants::calibration_speech_floor,
                  "the median's contribution must cap at the speech floor");
}

TEST_CASE("a user floor above the cap applies exactly as set") {
    // The cap clamps the median's CONTRIBUTION, not the whole max(): with
    // a user utterance floor of 1/32 above the cap and a fan-level median,
    // the gate is the user floor — capping the whole max() instead would
    // silently LOWER a user-set floor to 0.03, breaking the "floors are
    // minimums" invariant the config docs promise.
    UtteranceDetector detector(kSampleRate, 0.03125, 0.03125);

    // 1/64 is quiet relative to these floors and under the speech floor,
    // so it feeds the median; its contribution caps at 0.03, which the
    // user floor exceeds.
    for (int i = 0; i < 34; ++i)
        detector.process(constant_frame(0.015625f).data(), kFrameLen);
    CHECK_MESSAGE(detector.current_energy_threshold() == 0.03125,
                  "a user floor above the cap must win — floors are minimums");
}

// --- auto-stop silence accumulation (M1, wave-2) ----------------------------

TEST_CASE("continuous silence accumulates and resets on speech") {
    // The session's auto-stop reads continuous_silence and
    // is_currently_speaking. The counter must grow on every below-gate
    // frame INCLUDING silence while nobody ever spoke (the walked-away
    // case auto-stop exists for), and reset only on a speech frame.
    UtteranceDetector detector(kSampleRate, 0.010, 0.008);

    for (int i = 0; i < 10; ++i)
        detector.process(constant_frame(0.0f).data(), kFrameLen);
    CHECK(std::abs(detector.continuous_silence() - 0.3) < 0.001);
    CHECK(!detector.is_currently_speaking());

    detector.process(constant_frame(0.0625f).data(), kFrameLen);
    CHECK(detector.is_currently_speaking());
    CHECK(detector.continuous_silence() == 0.0);

    // Silence while the utterance is open still accumulates (auto-stop
    // waits for the utterance to close via is_currently_speaking, not by
    // freezing this counter).
    for (int i = 0; i < 5; ++i)
        detector.process(constant_frame(0.0f).data(), kFrameLen);
    CHECK(detector.is_currently_speaking());
    CHECK(std::abs(detector.continuous_silence() - 0.15) < 0.001);
}