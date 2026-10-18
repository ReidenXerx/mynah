// mynah-bird — the pill's bird as images, from the engine's own renderer.
//
// A dev tool for looking at what the pills will draw, without a pill:
//
//   mynah-bird out/ [size]    play a day in the bird's life at 60 fps and
//                             `size` px (default 128), into out/:
//                               wake-NNN.ppm   asleep, dictation starts and
//                                              the model loads
//                               start-NNN.ppm  the engine starts listening
//                               stop-NNN.ppm   dictation ends
//                               sleep-NNN.ppm  the model is unloaded
//   mynah-bird --pill speech.wav out/ [circle]
//                             the pill (the bird over its dark circle, the
//                             rim pulsing) while the engine listens to
//                             speech.wav — 16 kHz mono 16-bit — its voice
//                             measured as the engine measures it, at 60 fps,
//                             the circle `circle` px across (default 88):
//                             out/pill-NNN.ppm
//
// It drives the bird exactly as a pill does — the engine's state, its
// model status and a clock — so what it writes is what a pill shows. PPM,
// over the pill's dark grey, because it needs no library:
// `magick out/wake-*.ppm wake.gif`.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio/analysis.hpp"
#include "mynah/mynah.h"

namespace {

bool write_ppm(const std::string& path, const std::vector<uint32_t>& argb, int size) {
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    std::fprintf(out, "P6\n%d %d\n255\n", size, size);
    for (uint32_t px : argb) {
        // Premultiplied ARGB over #3b4252.
        const unsigned a = px >> 24, r = (px >> 16) & 255, g = (px >> 8) & 255, b = px & 255;
        const unsigned char rgb[3] = {(unsigned char)(r + (0x3b * (255 - a)) / 255),
                                      (unsigned char)(g + (0x42 * (255 - a)) / 255),
                                      (unsigned char)(b + (0x52 * (255 - a)) / 255)};
        std::fwrite(rgb, 1, 3, out);
    }
    return std::fclose(out) == 0;
}

// 16 kHz mono 16-bit PCM WAV samples, or empty (with a message) if not that.
std::vector<float> read_wav(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "mynah-bird: cannot read %s\n", path);
        return {};
    }
    std::vector<unsigned char> raw;
    unsigned char buffer[65536];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) raw.insert(raw.end(), buffer, buffer + n);
    std::fclose(f);
    auto u16 = [&](std::size_t at) { return unsigned(raw[at]) | unsigned(raw[at + 1]) << 8; };
    auto u32 = [&](std::size_t at) { return u16(at) | u16(at + 2) << 16; };
    std::size_t fmt = 0, data = 0, size = 0;
    for (std::size_t at = 12; at + 8 <= raw.size(); at += 8 + u32(at + 4)) {
        if (!std::memcmp(&raw[at], "fmt ", 4)) fmt = at + 8;
        if (!std::memcmp(&raw[at], "data", 4)) {
            data = at + 8;
            size = std::min<std::size_t>(u32(at + 4), raw.size() - data);
            break;
        }
    }
    if (raw.size() < 12 || std::memcmp(raw.data(), "RIFF", 4) || !fmt || !data || u16(fmt) != 1 ||
        u16(fmt + 2) != 1 || u32(fmt + 4) != unsigned(mynah::audio::kSampleRate) || u16(fmt + 14) != 16) {
        std::fprintf(stderr, "mynah-bird: %s is not 16 kHz mono 16-bit PCM\n", path);
        return {};
    }
    std::vector<float> samples(size / 2);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = float(int16_t(u16(data + 2 * i))) / 32768.0f;
    return samples;
}

// The pill while the engine listens to `samples`: the voice fed as the
// engine sends it, one 30 ms frame at a time, rendered at 60 fps over a
// dark circle like the front ends draw.
int pill(const std::vector<float>& samples, const std::string& dir, int circle) {
    const int width = int(std::ceil(circle / MYNAH_PILL_CIRCLE)), height = width;
    mynah_bird* bird = mynah_bird_create();
    if (!bird) return 1;
    mynah_bird_set_model(bird, MYNAH_MODEL_LOADED, -10.0);
    mynah_bird_set_state(bird, MYNAH_LISTENING, -5.0);
    mynah::audio::Spectrum spectrum;
    std::vector<uint32_t> argb(std::size_t(width) * height);
    const double seconds = double(samples.size()) / mynah::audio::kSampleRate;
    std::size_t fed = 0;
    int frames = 0;
    for (double now = 0; now < seconds; now += 1.0 / 60, ++frames) {
        for (; (fed + 1) * mynah::audio::kFrameSamples <= samples.size() &&
               double(fed + 1) * mynah::audio::kFrameSeconds <= now;
             ++fed) {
            const float* frame = &samples[fed * mynah::audio::kFrameSamples];
            float bands[MYNAH_SPECTRUM_BANDS];
            spectrum.compute(frame, mynah::audio::kFrameSamples, bands);
            const double level = mynah::audio::level(mynah::audio::rms(frame, mynah::audio::kFrameSamples));
            mynah_bird_set_voice(bird, float(level), bands, now);
        }
        if (mynah_bird_render_pill(bird, now, argb.data(), width, height) < 0) return 1;
        char name[32];
        std::snprintf(name, sizeof name, "/pill-%03d.ppm", frames);
        std::FILE* out = std::fopen((dir + name).c_str(), "wb");
        if (!out) return 1;
        std::fprintf(out, "P6\n%d %d\n255\n", width, height);
        const float radius = circle / 2.0f;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const uint32_t px = argb[std::size_t(y) * width + x];
                // #3b4252 desktop; the circle the HUD's dark (0.11 at 72%) over it.
                const float dx = x + 0.5f - width / 2.0f, dy = y + 0.5f - height / 2.0f;
                const bool in_circle = dx * dx + dy * dy <= radius * radius;
                const unsigned base[3] = {in_circle ? 0x1d : 0x3bu, in_circle ? 0x20 : 0x42u, in_circle ? 0x24 : 0x52u};
                const unsigned a = px >> 24;
                const unsigned char rgb[3] = {(unsigned char)(((px >> 16) & 255) + base[0] * (255 - a) / 255),
                                              (unsigned char)(((px >> 8) & 255) + base[1] * (255 - a) / 255),
                                              (unsigned char)((px & 255) + base[2] * (255 - a) / 255)};
                std::fwrite(rgb, 1, 3, out);
            }
        std::fclose(out);
    }
    mynah_bird_destroy(bird);
    std::printf("%s/: pill-*.ppm (%d frames), %dx%d, %.1f s of voice\n", dir.c_str(), frames, width, height, seconds);
    return 0;
}

// Render from `now` at 60 fps until the bird rests; the frame count, or -1.
int play(mynah_bird* bird, double& now, const std::string& prefix, std::vector<uint32_t>& argb, int size) {
    for (int frame = 0; frame < 600; ++frame, now += 1.0 / 60) {
        const int moving = mynah_bird_render(bird, now, argb.data(), size, size);
        char name[32];
        std::snprintf(name, sizeof name, "-%03d.ppm", frame);
        if (moving < 0 || !write_ppm(prefix + name, argb, size)) return -1;
        if (moving == 0) return frame + 1;
    }
    return -1; // never came to rest
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--pill") {
        const std::vector<float> samples = read_wav(argv[2]);
        const int circle = argc > 4 ? std::atoi(argv[4]) : 88;
        if (samples.empty() || circle <= 0 || circle > 1024) return 2;
        return pill(samples, argv[3], circle);
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: mynah-bird <out-dir> [size]\n       mynah-bird --pill <speech.wav> <out-dir> [circle]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const int size = argc > 2 ? std::atoi(argv[2]) : 128;
    if (size <= 0 || size > 4096) {
        std::fprintf(stderr, "mynah-bird: size %s is not 1..4096\n", argv[2]);
        return 2;
    }
    mynah_bird* bird = mynah_bird_create();
    if (!bird) {
        std::fprintf(stderr, "mynah-bird: the renderer could not start\n");
        return 1;
    }
    std::vector<uint32_t> argb(std::size_t(size) * size);
    double now = 0;
    int counts[4] = {-1, -1, -1, -1};
    // Asleep (the model is not loaded yet); a dictation starts: the engine
    // loads, the model with it. The wake plays; LISTENING comes after it.
    mynah_bird_set_state(bird, MYNAH_LOADING, now);
    mynah_bird_set_model(bird, MYNAH_MODEL_LOADING, now);
    counts[0] = play(bird, now, dir + "/wake", argb, size);
    mynah_bird_set_model(bird, MYNAH_MODEL_LOADED, now);
    mynah_bird_set_state(bird, MYNAH_LISTENING, now);
    if (counts[0] > 0) counts[1] = play(bird, now, dir + "/start", argb, size);
    mynah_bird_set_state(bird, MYNAH_TRANSCRIBING, now);
    if (counts[1] > 0) counts[2] = play(bird, now, dir + "/stop", argb, size);
    // Idle; the idle timeout unloads the model, and the bird falls asleep.
    mynah_bird_set_state(bird, MYNAH_IDLE, now);
    mynah_bird_set_model(bird, MYNAH_MODEL_UNLOADED, now);
    if (counts[2] > 0) counts[3] = play(bird, now, dir + "/sleep", argb, size);
    mynah_bird_destroy(bird);
    if (counts[3] < 0) {
        std::fprintf(stderr, "mynah-bird: rendering into %s/ failed\n", dir.c_str());
        return 1;
    }
    std::printf("%s/: wake (%d frames), start (%d), stop (%d), sleep (%d), %d px\n", dir.c_str(), counts[0],
                counts[1], counts[2], counts[3], size);
    return 0;
}
