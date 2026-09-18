// Level and spectrum analysis for the indicator, ported from engine.py's
// callback math: level = min(1, rms * 5), and the 12-band spectrum built
// from _band_edges + _spectrum over a Hann-windowed frame.
//
// engine.py evaluates the spectrum with numpy's rfft over the 480-sample
// frame (480 = 2^5 * 15, not a power of two, so there is no textbook radix-2
// FFT for it). The same DFT bins via Goertzel per bin are exact, need no
// FFT implementation, and only the bins under the 12 bands are ever
// computed — cheaper per frame than a full transform on the audio thread.

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "tuning/constants.hpp"

namespace mynah::audio {

// Whisper is trained on 16 kHz mono; the front end pushes that.
inline constexpr int kSampleRate = 16000;
// The contract's frame: 30 ms at 16 kHz — the golden corpus's frame size
// and the spectrum's window size.
inline constexpr int kFrameSamples = 480;
inline constexpr double kFrameSeconds =
    static_cast<double>(kFrameSamples) / kSampleRate;

// RMS amplitude of normalized float samples, 0..1.
double rms(const float* samples, std::size_t count);

// The indicator volume curve: rms scaled so normal speech is near full
// scale (engine.py: min(1.0, rms * 5.0)).
inline double level(double rms_value) { return rms_value * 5.0 < 1.0 ? rms_value * 5.0 : 1.0; }

// The 12-band spectrum of one frame, engine.py's math exactly: Hann
// window, per-bin magnitudes normalized by the window's own sum, band
// means, sqrt-scaled against spectrum_reference, each clamped to [0, 1].
class Spectrum {
public:
    Spectrum(int frame_samples = kFrameSamples);

    // bands must have room for MYNAH_SPECTRUM_BANDS values.
    void compute(const float* frame, std::size_t count, float* bands) const;

private:
    int frame_samples_;
    // Per-bin Goertzel coefficients: precomputed cos/sin for k * 2π / N.
    std::vector<double> coeff_;      // 2 * cos(ω)
    std::vector<double> window_;     // the Hann window
    double window_sum_ = 0.0;
    // Band edges as bin ranges into the magnitude array, low to high.
    std::vector<std::pair<std::size_t, std::size_t>> edges_;
};

} // namespace mynah::audio