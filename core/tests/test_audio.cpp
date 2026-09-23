// The audio layer: the SPSC ring buffer (the push_audio contract: a stalled
// worker must cost audio, never the capture thread) and the spectrum
// analysis (engine.py's 12-band meter math, via Goertzel instead of
// numpy's rfft — the same DFT bins either way).

#include "vendor/doctest.h"

#include <atomic>
#include <cmath>
#include <numeric>
#include <thread>
#include <vector>

#include "audio/analysis.hpp"
#include "audio/ring_buffer.hpp"

using mynah::audio::RingBuffer;
using mynah::audio::Spectrum;
constexpr int kBands = mynah::constants::spectrum_bands; // pinned == MYNAH_SPECTRUM_BANDS

TEST_CASE("ring buffer: FIFO order across wraparound") {
    RingBuffer ring(100); // rounds up to 128
    std::vector<float> pushed(2000);
    std::iota(pushed.begin(), pushed.end(), 0.0f);
    std::size_t written = 0;   // accepted by the ring so far
    std::size_t delivered = 0; // verified by the reader so far
    std::vector<float> out(16);

    // Write and read in mismatched chunks so the indices wrap. Every pop
    // drains everything readable, so nothing is dropped and the invariant
    // "delivered samples are exactly the accepted ones, in order" holds.
    // The final push clips to the vector — push copies `count` samples,
    // and reading past the end would put garbage in the ring.
    while (delivered < pushed.size()) {
        if (written < pushed.size()) {
            std::size_t chunk = std::min(std::size_t(37), pushed.size() - written);
            written += ring.push(pushed.data() + written, chunk);
        }
        std::size_t popped = ring.pop(out.data(), out.size());
        for (std::size_t i = 0; i < popped; ++i)
            REQUIRE(out[i] == float(delivered + i));
        delivered += popped;
    }
    CHECK(ring.readable() == 0);
}

TEST_CASE("ring buffer: a full ring drops, and counts what it dropped") {
    RingBuffer ring(64); // capacity 64 exactly
    std::vector<float> samples(100, 1.0f);
    std::size_t accepted = ring.push(samples.data(), samples.size());
    CHECK(accepted == 64);
    CHECK(ring.dropped() == 36);

    // The accepted part is intact; drain everything.
    std::vector<float> out(64);
    CHECK(ring.pop(out.data(), out.size()) == 64);
    for (float sample : out) CHECK(sample == 1.0f);
    CHECK(ring.push(samples.data(), 1) == 1); // space again
    CHECK(ring.dropped() == 36);
}

TEST_CASE("ring buffer: one producer, one consumer, nothing lost or duplicated") {
    RingBuffer ring(1024);
    constexpr std::size_t kTotal = 100000;
    std::vector<float> produced(kTotal);
    std::iota(produced.begin(), produced.end(), 0.0f);
    std::atomic<std::size_t> consumed_count{0};
    std::uint64_t checksum = 0;

    std::thread consumer([&] {
        std::vector<float> chunk(97); // prime size: constant re-wrapping
        while (true) {
            std::size_t popped = ring.pop(chunk.data(), chunk.size());
            for (std::size_t i = 0; i < popped; ++i) {
                checksum += std::uint64_t(chunk[i]);
                consumed_count.fetch_add(1);
            }
            if (consumed_count.load() >= kTotal) break;
        }
    });
    std::size_t written = 0;
    while (written < kTotal) {
        // A retrying producer can see partial accepts (counted as drops by
        // the ring — the capture-thread caller abandons what push refuses);
        // the test clips its chunks and asserts only data integrity.
        std::size_t chunk = std::min(std::size_t(61), kTotal - written);
        std::size_t n = ring.push(produced.data() + written, chunk);
        written += n;
    }
    consumer.join();

    CHECK(consumed_count.load() == kTotal); // no loss, no duplication
    // Sum in integers: accumulate(uint64, float) promotes to float and
    // loses precision past 2^24, and the exact sum is 5e9.
    std::uint64_t expected = 0;
    for (float value : produced) expected += std::uint64_t(value);
    CHECK(checksum == expected);
}

TEST_CASE("rms is normalized 0..1 and matches hand math") {
    std::vector<float> silence(480, 0.0f);
    CHECK(mynah::audio::rms(silence.data(), silence.size()) == 0.0);

    std::vector<float> constant(480, 0.0625f);
    CHECK(std::abs(mynah::audio::rms(constant.data(), constant.size()) - 0.0625) < 1e-12);

    // Half ones, half zeros: rms = sqrt(0.5).
    std::vector<float> alternating(480);
    for (int i = 0; i < 480; ++i) alternating[i] = i % 2 ? 1.0f : 0.0f;
    CHECK(std::abs(mynah::audio::rms(alternating.data(), alternating.size()) -
                   std::sqrt(0.5)) < 1e-12);

    CHECK(mynah::audio::level(0.1) == doctest::Approx(0.5));
    CHECK(mynah::audio::level(0.5) == 1.0); // min(1, 5*rms)
}

TEST_CASE("the spectrum puts a sine's energy in its own band") {
    Spectrum spectrum;
    float bands[kBands];

    // Silence reads as zero everywhere.
    std::vector<float> silence(480, 0.0f);
    spectrum.compute(silence.data(), silence.size(), bands);
    for (int i = 0; i < kBands; ++i) CHECK(bands[i] == 0.0f);

    // A 200 Hz sine at amplitude 0.0625. The band edges are int-truncated
    // log-spaced bins (engine.py's _band_edges): band 3 spans bins 6..8
    // (200..266 Hz) and owns bin 6 = 200 Hz exactly; band 2 catches the
    // Hann window's mainlobe leakage.
    std::vector<float> sine(480);
    for (int i = 0; i < 480; ++i)
        sine[i] = 0.0625f * std::sin(2.0 * 3.14159265358979 * 200.0 * i / 16000.0);
    spectrum.compute(sine.data(), sine.size(), bands);
    int peak = 0;
    for (int i = 1; i < kBands; ++i)
        if (bands[i] > bands[peak]) peak = i;
    CHECK(peak == 3);
    // A constant-amplitude sine of RMS 0.0625/sqrt(2) = 0.044 is near the
    // spectrum reference 0.03 — the band reads near full scale.
    CHECK(bands[peak] > 0.5f);
    CHECK(bands[peak] <= 1.0f);
    CHECK(bands[2] > 0.0f);  // leakage into the neighbour, not a void

    // 1 kHz lands in band 7 (bins 26..36, 866..1200 Hz) — the log spacing
    // puts three decades of speech in twelve bands.
    for (int i = 0; i < 480; ++i)
        sine[i] = 0.0625f * std::sin(2.0 * 3.14159265358979 * 1000.0 * i / 16000.0);
    spectrum.compute(sine.data(), sine.size(), bands);
    peak = 0;
    for (int i = 1; i < kBands; ++i)
        if (bands[i] > bands[peak]) peak = i;
    CHECK(peak == 7);

    // A short tail frame reads as silence, not garbage.
    spectrum.compute(sine.data(), 100, bands);
    for (int i = 0; i < kBands; ++i) CHECK(bands[i] == 0.0f);
}
TEST_CASE("skip_to discards up to a recorded write position, never past it") {
    mynah::audio::RingBuffer ring(8);
    const float a[] = {1, 2, 3};
    const float b[] = {4, 5};
    ring.push(a, 3);
    const std::size_t mark = ring.written(); // a session armed here
    ring.push(b, 2);
    CHECK(ring.skip_to(mark) == 3);
    float out[4] = {};
    REQUIRE(ring.pop(out, 4) == 2);
    CHECK(out[0] == 4);
    CHECK(out[1] == 5);
    // Already read past: nothing to skip. Beyond what is written: clamped.
    CHECK(ring.skip_to(mark) == 0);
    ring.push(a, 3);
    CHECK(ring.skip_to(ring.written() + 100) == 3);
    CHECK(ring.readable() == 0);
}
