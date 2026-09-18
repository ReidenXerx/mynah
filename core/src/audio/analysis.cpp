#include "analysis.hpp"

#include <cmath>
#include <utility>
#include <vector>

namespace mynah::audio {

namespace {
// std::cmath is not required to define M_PI; this is the same constant.
constexpr double kPi = 3.14159265358979323846;
}

double rms(const float* samples, std::size_t count) {
    if (count == 0) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) sum += double(samples[i]) * samples[i];
    return std::sqrt(sum / double(count));
}

Spectrum::Spectrum(int frame_samples)
    : frame_samples_(frame_samples), window_(frame_samples, 0.0) {
    // The Hann window, matching np.hanning(N): 0.5 * (1 - cos(2π i / N)).
    double window_sum = 0.0;
    for (int i = 0; i < frame_samples; ++i) {
        window_[i] = 0.5 * (1.0 - std::cos(2.0 * kPi * double(i) / double(frame_samples)));
        window_sum += window_[i];
    }
    window_sum_ = window_sum;

    // _band_edges from engine.py: log-spaced edges in hertz, each band
    // owning at least one bin ("Every band must own at least one bin, or
    // the low end is empty").
    std::vector<int> edges;
    for (int band = 0; band <= constants::spectrum_bands; ++band) {
        double fraction = double(band) / constants::spectrum_bands;
        double hz = constants::spectrum_low_hz *
                    std::pow(constants::spectrum_high_hz / constants::spectrum_low_hz, fraction);
        edges.push_back(int(hz * frame_samples_ / kSampleRate));
    }
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        std::size_t lo = static_cast<std::size_t>(edges[i] > 0 ? edges[i] : 0);
        std::size_t hi = static_cast<std::size_t>(edges[i + 1]);
        if (hi <= lo) hi = lo + 1;
        edges_.emplace_back(lo, hi);
    }

    // Goertzel coefficients for every bin a band can touch.
    std::size_t max_bin = 0;
    for (const auto& [lo, hi] : edges_) max_bin = hi > max_bin ? hi : max_bin;
    coeff_.resize(max_bin + 2);
    for (std::size_t k = 0; k < coeff_.size(); ++k) {
        double omega = 2.0 * kPi * double(k) / double(frame_samples_);
        coeff_[k] = 2.0 * std::cos(omega); // 2·cos(ω)
    }
}

void Spectrum::compute(const float* frame, std::size_t count, float* bands) const {
    if (count != static_cast<std::size_t>(frame_samples_)) {
        // The meter is cosmetic; a short tail frame reads as silence.
        for (int i = 0; i < constants::spectrum_bands; ++i) bands[i] = 0.0f;
        return;
    }

    // Per-bin magnitudes via Goertzel on the windowed frame. Goertzel
    // evaluates the same DFT sum numpy's rfft does (un-normalized), so
    // dividing by the window's own sum/2 lands each band in the signal's
    // amplitude units — engine.py: |rfft(mono * window)| / (sum/2).
    std::vector<double> magnitude(coeff_.size(), 0.0);
    for (std::size_t k = 0; k < coeff_.size(); ++k) {
        double s_prev = 0.0;
        double s_prev2 = 0.0;
        double coeff = coeff_[k];
        for (int n = 0; n < frame_samples_; ++n) {
            double sample = double(frame[n]) * window_[n];
            double s = sample + coeff * s_prev - s_prev2;
            s_prev2 = s_prev;
            s_prev = s;
        }
        // |X(k)| = sqrt(s1² + s2² − 2·cos(ω)·s1·s2).
        magnitude[k] =
            std::sqrt(s_prev * s_prev + s_prev2 * s_prev2 - coeff * s_prev * s_prev2) /
            (window_sum_ / 2.0);
    }

    for (int b = 0; b < constants::spectrum_bands; ++b) {
        auto [lo, hi] = edges_[b];
        double sum = 0.0;
        for (std::size_t k = lo; k < hi && k < magnitude.size(); ++k) sum += magnitude[k];
        double value = hi > lo ? sum / double(hi - lo) : 0.0;
        // Square root rather than raw magnitude: quiet consonants are most
        // of what makes a meter look alive, and linear scaling hides them.
        bands[b] = value > 0.0
                      ? static_cast<float>(std::min(1.0, std::sqrt(value / constants::spectrum_reference)))
                      : 0.0f;
    }
}

} // namespace mynah::audio