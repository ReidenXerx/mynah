// mynah::segment — segments a continuous mic stream into utterances.
//
// THE migration policy (P2, docs/ENGINE-MIGRATION.md): this is a port of
// Swift's UtteranceDetector, which trims trailing silence to
// trailing_padding when closing an utterance. Python kept all of it — one
// of the two documented divergences the single engine exists to remove.
// The golden corpus (tuning/golden/) pins the shared boundaries; the C++
// tests in core/tests/test_detector.cpp are the twins of
// macos/Tests/MynahAppTests/TuningTests.swift and
// tests/test_segmentation_golden.py.
//
// Two gates in series, and the order matters (the Python engine learned
// this the hard way): the per-frame energy floor runs first, so steady
// low-level noise never reaches the speech decision — webrtcvad once
// classified MacBook fan noise as speech and the utterances that passed
// the whole-buffer RMS check came back as subtitle-credit hallucinations.
//
// Speech-aware calibration (wave-2, M13): per-frame RMS is collected over
// the first noise_calibration_seconds of a session, speech-energy frames
// are excluded from the median, and the median's CONTRIBUTION is capped at
// calibration_speech_floor — a calibrated gate above the speech/noise
// discrimination line would demand speech louder than speech, and no static
// floor setting could counter it (the floors are minimums).

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "tuning/constants.hpp"

namespace mynah::segment {

// A closed utterance: 16 kHz mono samples with the trailing silence
// already trimmed to constants::trailing_padding (the P2 policy).
struct Utterance {
    std::vector<float> samples;
    double duration = 0.0; // seconds, of the trimmed samples
};

class UtteranceDetector {
public:
    // Static floors come from the config so sensitivity is tunable without
    // a rebuild; the calibration raises them, never lowers them.
    UtteranceDetector(double sample_rate, double frame_floor, double utterance_floor)
        : sample_rate_(sample_rate), frame_floor_(frame_floor),
          utterance_floor_(utterance_floor), frame_threshold_(frame_floor),
          utterance_threshold_(utterance_floor) {}

    // Feed one frame. Returns an utterance when silence closes one.
    // Frames may be any size; the 30 ms contract frame is the norm.
    std::optional<Utterance> process(const float* frame, std::size_t count);

    // Close out whatever is buffered — the session end, so a final
    // utterance is not lost to the user releasing the key mid-sentence.
    std::optional<Utterance> flush();

    // The utterance energy gate, raised if the room turned out noisy.
    double current_energy_threshold() const { return utterance_threshold_; }

    // Continuous silence in seconds since the last speech frame — the input
    // to auto-stop. Accumulates on every below-gate frame, including when
    // nobody ever spoke (the walked-away case), and resets only on speech.
    double continuous_silence() const { return silence_since_speech_; }

    // Whether an utterance is currently open. Auto-stop must not fire
    // mid-utterance.
    bool is_currently_speaking() const { return is_speaking_; }

private:
    void apply_calibration();
    Utterance make_utterance() const;
    void reset_buffer();

    double sample_rate_;
    std::vector<float> buffer_;
    double silent_duration_ = 0.0;      // trailing silence inside an open utterance
    double silence_since_speech_ = 0.0; // auto-stop accumulator
    bool is_speaking_ = false;
    // Calibration window, measured in seconds of accumulated audio (the
    // frame that completes the window is itself measured against the
    // resulting gates — engine.py calls _finish_noise_calibration before
    // the segmentation check).
    std::vector<double> calibration_samples_;
    double calibrated_duration_ = 0.0;
    double frame_floor_;
    double utterance_floor_;
    double frame_threshold_;
    double utterance_threshold_;
};

} // namespace mynah::segment