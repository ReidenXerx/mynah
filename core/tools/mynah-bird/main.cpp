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
//
// It drives the bird exactly as a pill does — the engine's state, its
// model status and a clock — so what it writes is what a pill shows. PPM,
// over the pill's dark grey, because it needs no library:
// `magick out/wake-*.ppm wake.gif`.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
    if (argc < 2) {
        std::fprintf(stderr, "usage: mynah-bird <out-dir> [size]\n");
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
