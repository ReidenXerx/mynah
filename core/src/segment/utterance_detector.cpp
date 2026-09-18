#include "utterance_detector.hpp"

#include <algorithm>

namespace mynah::segment {

std::optional<Utterance> UtteranceDetector::process(const float* frame, std::size_t count) {
    if (count == 0) return std::nullopt;

    double frame_duration = double(count) / sample_rate_;
    // The gate comparison runs on the frame's whole-buffer RMS, exactly as
    // engine.py's callback and UtteranceDetector.swift do.
    double energy = 0.0;
    for (std::size_t i = 0; i < count; ++i) energy += double(frame[i]) * frame[i];
    energy = std::sqrt(energy / double(count));

    // Sample the ambient level, but keep processing this frame. Returning
    // nothing during the calibration window threw away the first second of
    // every session — anyone who pressed the hotkey and started talking
    // immediately lost their opening word. During the window the static
    // floors are in force, exactly as in Python.
    if (calibrated_duration_ < constants::noise_calibration_seconds) {
        calibration_samples_.push_back(energy);
        calibrated_duration_ += frame_duration;
        if (calibrated_duration_ >= constants::noise_calibration_seconds)
            apply_calibration();
    }

    if (energy >= frame_threshold_) {
        is_speaking_ = true;
        silent_duration_ = 0.0;
        silence_since_speech_ = 0.0;
        buffer_.insert(buffer_.end(), frame, frame + count);
        return std::nullopt;
    }

    // Below the frame gate: silence. It accumulates into the auto-stop
    // counter whether or not an utterance is open.
    silence_since_speech_ += frame_duration;

    if (!is_speaking_) return std::nullopt;

    // Trailing silence still belongs to the utterance — cutting at the
    // exact frame speech drops clips word endings.
    buffer_.insert(buffer_.end(), frame, frame + count);
    silent_duration_ += frame_duration;
    if (silent_duration_ < constants::utterance_silence) return std::nullopt;

    Utterance utterance = make_utterance();
    reset_buffer();
    return utterance;
}

Utterance UtteranceDetector::make_utterance() const {
    // The buffer holds every silent frame up to the 0.8 s threshold, so a
    // short phrase is ~0.1 s of speech followed by 0.8 s of nothing.
    // Gating on RMS across all of that averages the speech away and the
    // utterance is silently discarded — so trim to trailing_padding.
    // (The P2 policy: min_utterance is applied to THIS trimmed duration.)
    int keep = int(constants::trailing_padding * sample_rate_);
    int drop = std::max(0, int(silent_duration_ * sample_rate_) - keep);
    std::vector<float> trimmed;
    if (drop > 0 && std::size_t(drop) < buffer_.size())
        trimmed.assign(buffer_.begin(), buffer_.end() - drop);
    else
        trimmed = buffer_;
    double duration = double(trimmed.size()) / sample_rate_;
    return Utterance{std::move(trimmed), duration};
}

std::optional<Utterance> UtteranceDetector::flush() {
    if (!is_speaking_ || buffer_.empty()) return std::nullopt;
    Utterance utterance = make_utterance();
    reset_buffer();
    return utterance;
}

void UtteranceDetector::reset_buffer() {
    buffer_.clear();
    silent_duration_ = 0.0;
    is_speaking_ = false;
}

void UtteranceDetector::apply_calibration() {
    // Speech-aware: frames at or above calibration_speech_floor are
    // speech, not noise — excluded from the median. Fewer than
    // noise_min_samples quiet frames aborts, leaving the static gates in
    // force: the median must never be measured on speech (that poisoned
    // the floor and silently dropped the first word).
    std::vector<double> quiet;
    quiet.reserve(calibration_samples_.size());
    for (double energy : calibration_samples_)
        if (energy < constants::calibration_speech_floor) quiet.push_back(energy);
    if (quiet.size() < std::size_t(constants::noise_min_samples)) return;

    std::sort(quiet.begin(), quiet.end());
    std::size_t n = quiet.size();
    // The averaging median — engine.py's convention, and the boundary the
    // tests pin. (Swift once took the upper-middle element for even
    // counts; a divergence the uniform fixtures cannot see, pinned by the
    // median tests in both suites.)
    double median = n % 2 == 1 ? quiet[n / 2] : (quiet[n / 2 - 1] + quiet[n / 2]) / 2.0;

    // Cap the median's CONTRIBUTION at the speech floor (M13): a
    // calibrated gate above the speech/noise line would demand speech
    // louder than speech, and no frame_energy/min_energy setting could
    // take effect against it. The cap clamps the contribution, not the
    // whole max, so a user floor above the cap applies exactly as set —
    // and it cannot misfire: the median is measured only on quiet frames,
    // so the contribution only ever rises toward the cap from below.
    double cap = constants::calibration_speech_floor;
    frame_threshold_ =
        std::max(frame_floor_, std::min(median * constants::noise_frame_multiplier, cap));
    utterance_threshold_ =
        std::max(utterance_floor_,
                 std::min(median * constants::noise_utterance_multiplier, cap));
}

} // namespace mynah::segment