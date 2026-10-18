// The pill's bird (mynah_bird_*): the embedded Lottie loads, each engine
// state lands in the right pose, transitions play and then rest, a change
// mid-transition turns back without a jump, and reduced motion skips the
// motion. Checked on the pixels: where the raised wing is drawn.

#include "vendor/doctest.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include "mynah/mynah.h"

namespace {

constexpr int kSize = 128;

using BirdPtr = std::unique_ptr<mynah_bird, decltype(&mynah_bird_destroy)>;

BirdPtr make_bird() { return BirdPtr(mynah_bird_create(), &mynah_bird_destroy); }

struct Frame {
    int moving = -1;
    std::vector<uint32_t> argb;
};

Frame render(mynah_bird* bird, double now, int width = kSize, int height = kSize) {
    Frame frame;
    frame.argb.assign(std::size_t(width) * height, 0xdeadbeef);
    frame.moving = mynah_bird_render(bird, now, frame.argb.data(), width, height);
    return frame;
}

// Opaque pixels inside a region of a kSize render, as a fraction.
double coverage(const Frame& frame, double x0, double y0, double x1, double y1) {
    int painted = 0, total = 0;
    for (int y = int(y0 * kSize); y < int(y1 * kSize); ++y)
        for (int x = int(x0 * kSize); x < int(x1 * kSize); ++x) {
            ++total;
            if ((frame.argb[std::size_t(y) * kSize + x] >> 24) > 128) ++painted;
        }
    return total ? double(painted) / total : 0.0;
}

// Where the raised wing is drawn once it is up — upper right of the bird.
// Empty in the idle pose: that wing is faded out there.
double wing(const Frame& frame) { return coverage(frame, 0.78, 0.30, 0.90, 0.45); }
// The middle of the body, drawn in every pose.
double body(const Frame& frame) { return coverage(frame, 0.35, 0.35, 0.65, 0.65); }

// Pixels that differ visibly between two renders, as a fraction.
double difference(const Frame& a, const Frame& b) {
    int differ = 0;
    for (std::size_t i = 0; i < a.argb.size(); ++i)
        for (int shift = 0; shift < 32; shift += 8)
            if (std::abs(int((a.argb[i] >> shift) & 255) - int((b.argb[i] >> shift) & 255)) > 48) {
                ++differ;
                break;
            }
    return double(differ) / double(a.argb.size());
}

} // namespace

TEST_CASE("the bird starts at rest, in the idle pose") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    const Frame frame = render(bird.get(), 0.0);
    CHECK(frame.moving == 0);
    CHECK(body(frame) > 0.9);
    CHECK(wing(frame) < 0.2);
}

TEST_CASE("dictation starting raises the wing, then the bird rests") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 10.0);
    CHECK(render(bird.get(), 10.25).moving == 1);
    const Frame up = render(bird.get(), 11.0); // the start transition is 0.5 s
    CHECK(up.moving == 0);
    CHECK(body(up) > 0.9);
    CHECK(wing(up) > 0.5);
}

TEST_CASE("loading is the listening pose: the press already records") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LOADING, 0.0);
    CHECK(render(bird.get(), 0.25).moving == 1);
    // The model is up: no second transition, the wing stays where it is.
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 2.0);
    const Frame frame = render(bird.get(), 2.0);
    CHECK(frame.moving == 0);
    CHECK(wing(frame) > 0.5);
}

TEST_CASE("dictation ending lowers the wing; transcribing and idle are both the resting pose") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    REQUIRE(wing(render(bird.get(), 1.0)) > 0.5);
    mynah_bird_set_state(bird.get(), MYNAH_TRANSCRIBING, 5.0);
    CHECK(render(bird.get(), 5.3).moving == 1);
    const Frame down = render(bird.get(), 6.0); // the stop transition is 0.67 s
    CHECK(down.moving == 0);
    CHECK(wing(down) < 0.2);
    mynah_bird_set_state(bird.get(), MYNAH_IDLE, 7.0);
    CHECK(render(bird.get(), 7.0).moving == 0);
}

TEST_CASE("a stop halfway through the start turns back from the very frame it is on") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    const Frame before = render(bird.get(), 0.2);
    REQUIRE(before.moving == 1);
    mynah_bird_set_state(bird.get(), MYNAH_IDLE, 0.2);
    const Frame after = render(bird.get(), 0.2);
    // The same frame, so no jump. (Starting the stop transition at the same
    // fraction instead changed 2.7% of the pixels in one frame — more than
    // half the whole idle-to-listening change, 4.8%.)
    CHECK(difference(before, after) == 0.0);
    CHECK(after.moving == 1);
    // Backwards over the 0.2 s it had played: idle again at 0.4 s.
    CHECK(render(bird.get(), 0.35).moving == 1);
    const Frame back = render(bird.get(), 0.41);
    CHECK(back.moving == 0);
    CHECK(wing(back) < 0.2);
}

TEST_CASE("a start halfway through the stop turns back too, to the listening pose") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    REQUIRE(render(bird.get(), 1.0).moving == 0);
    mynah_bird_set_state(bird.get(), MYNAH_TRANSCRIBING, 1.0);
    const Frame before = render(bird.get(), 1.3);
    REQUIRE(before.moving == 1);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 1.3);
    CHECK(difference(before, render(bird.get(), 1.3)) == 0.0);
    const Frame up = render(bird.get(), 1.61);
    CHECK(up.moving == 0);
    CHECK(wing(up) > 0.5);
    // And from rest, the designed start transition again, not a reversal.
    mynah_bird_set_state(bird.get(), MYNAH_IDLE, 3.0);
    CHECK(render(bird.get(), 3.5).moving == 1); // the stop is 0.67 s
    CHECK(render(bird.get(), 3.7).moving == 0);
}

TEST_CASE("reduced motion: changes jump to their end pose") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_reduced_motion(bird.get(), 1);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    const Frame up = render(bird.get(), 0.0);
    CHECK(up.moving == 0);
    CHECK(wing(up) > 0.5);
    mynah_bird_set_state(bird.get(), MYNAH_IDLE, 0.1);
    const Frame down = render(bird.get(), 0.1);
    CHECK(down.moving == 0);
    CHECK(wing(down) < 0.2);
}

TEST_CASE("the pixels are premultiplied ARGB, and every one is written") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    const Frame frame = render(bird.get(), 1.0); // filled with 0xdeadbeef first
    int bad = 0;
    for (uint32_t px : frame.argb) {
        const uint32_t a = px >> 24;
        if (((px >> 16) & 255) > a || ((px >> 8) & 255) > a || (px & 255) > a) ++bad;
    }
    CHECK(bad == 0);
}

TEST_CASE("any size: fitted and centred, keeping the aspect") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    const Frame wide = render(bird.get(), 0.0, 200, 100);
    REQUIRE(wide.moving == 0);
    int outside = 0, middle = 0;
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 200; ++x) {
            const bool on = (wide.argb[std::size_t(y) * 200 + x] >> 24) > 128;
            if ((x < 50 || x >= 150) && on) ++outside; // the 100 px square sits in the middle
            if (x >= 80 && x < 120 && y >= 40 && y < 60 && on) ++middle;
        }
    CHECK(outside == 0);
    CHECK(middle > 600);
    // And back to a square after: the canvas re-targets per size.
    CHECK(body(render(bird.get(), 0.0)) > 0.9);
}

TEST_CASE("bad arguments are refused, not crashed on") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    uint32_t pixel = 0;
    CHECK(mynah_bird_render(nullptr, 0.0, &pixel, 1, 1) == -1);
    CHECK(mynah_bird_render(bird.get(), 0.0, nullptr, 1, 1) == -1);
    CHECK(mynah_bird_render(bird.get(), 0.0, &pixel, 0, 1) == -1);
    mynah_bird_set_state(nullptr, MYNAH_LISTENING, 0.0);
    mynah_bird_set_reduced_motion(nullptr, 1);
    mynah_bird_destroy(nullptr);
}
