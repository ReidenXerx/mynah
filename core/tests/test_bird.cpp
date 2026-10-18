// The pill's bird (mynah_bird_*): the embedded Lottie loads, each engine
// state and model status lands in the right pose — asleep, idle, listening
// — transitions play and then rest, the way from asleep to listening goes
// through idle, a change mid-transition turns back without a jump, and
// reduced motion skips the motion. Checked on the pixels: where the raised
// wing is drawn, and where the big Z is.

#include "vendor/doctest.h"

#include <algorithm>
#include <cmath>
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
// Where the big Z is drawn while asleep, up and right of the head; empty awake.
double zzz(const Frame& frame) { return coverage(frame, 0.79, 0.12, 0.88, 0.20); }

// A bird that is awake and idle by t = 0: the model loaded a while ago.
BirdPtr awake_bird() {
    BirdPtr bird = make_bird();
    if (bird) mynah_bird_set_model(bird.get(), MYNAH_MODEL_LOADED, -10.0);
    return bird;
}

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

TEST_CASE("the bird starts asleep: nothing is loaded before the first dictation") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    const Frame frame = render(bird.get(), 0.0);
    CHECK(frame.moving == 0);
    CHECK(body(frame) > 0.9);
    CHECK(wing(frame) < 0.2);
    CHECK(zzz(frame) > 0.15);
}

TEST_CASE("a model that has loaded wakes the bird, and the idle unload puts it back to sleep") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_model(bird.get(), MYNAH_MODEL_LOADED, 0.0);
    CHECK(render(bird.get(), 0.4).moving == 1);
    const Frame up = render(bird.get(), 1.0); // the wake is 0.8 s
    CHECK(up.moving == 0);
    CHECK(zzz(up) < 0.02);
    CHECK(wing(up) < 0.2); // awake is not listening
    mynah_bird_set_model(bird.get(), MYNAH_MODEL_UNLOADED, 5.0);
    CHECK(render(bird.get(), 5.6).moving == 1);
    const Frame down = render(bird.get(), 6.5); // falling asleep is 1.33 s
    CHECK(down.moving == 0);
    CHECK(zzz(down) > 0.15);
}

TEST_CASE("asleep, dictation starting wakes the bird; the wing goes up only once the engine listens") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    // The press: the engine loads, and the model with it — the GPU waking.
    mynah_bird_set_state(bird.get(), MYNAH_LOADING, 0.0);
    mynah_bird_set_model(bird.get(), MYNAH_MODEL_LOADING, 0.0);
    CHECK(render(bird.get(), 0.3).moving == 1);
    const Frame woke = render(bird.get(), 1.0);
    CHECK(woke.moving == 0); // awake, still loading: not listening yet
    CHECK(zzz(woke) < 0.02);
    CHECK(wing(woke) < 0.2);
    mynah_bird_set_model(bird.get(), MYNAH_MODEL_LOADED, 1.5);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 1.5);
    CHECK(render(bird.get(), 1.7).moving == 1);
    const Frame up = render(bird.get(), 2.1);
    CHECK(up.moving == 0);
    CHECK(wing(up) > 0.5);
}

TEST_CASE("a model that loads before the bird is awake: the wing waits for the wake, then goes up") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LOADING, 0.0);
    mynah_bird_set_model(bird.get(), MYNAH_MODEL_LOADED, 0.1);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.2);
    const Frame waking = render(bird.get(), 0.5);
    CHECK(waking.moving == 1);
    CHECK(wing(waking) < 0.2); // still waking: the wing has not started
    // The wake ends at 0.8 s and the 0.5 s start follows from then — even
    // with no render in between to notice the hand-over.
    CHECK(render(bird.get(), 1.0).moving == 1);
    const Frame up = render(bird.get(), 1.4);
    CHECK(up.moving == 0);
    CHECK(wing(up) > 0.5);
}

TEST_CASE("a press while the bird falls asleep turns back from the very frame it is on") {
    BirdPtr bird = awake_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_model(bird.get(), MYNAH_MODEL_UNLOADED, 0.0);
    const Frame before = render(bird.get(), 0.4);
    REQUIRE(before.moving == 1);
    mynah_bird_set_state(bird.get(), MYNAH_LOADING, 0.4);
    CHECK(difference(before, render(bird.get(), 0.4)) == 0.0);
    // Backwards over the 0.4 s it had played: awake again at 0.8 s, with no
    // Z's — not the whole wake transition after a whole fall.
    CHECK(render(bird.get(), 0.75).moving == 1);
    const Frame back = render(bird.get(), 0.81);
    CHECK(back.moving == 0);
    CHECK(zzz(back) < 0.02);
}

TEST_CASE("dictation starting raises the wing, then the bird rests") {
    BirdPtr bird = awake_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 10.0);
    CHECK(render(bird.get(), 10.25).moving == 1);
    const Frame up = render(bird.get(), 11.0); // the start transition is 0.5 s
    CHECK(up.moving == 0);
    CHECK(body(up) > 0.9);
    CHECK(wing(up) > 0.5);
}

TEST_CASE("awake, loading is not listening: the wing waits for LISTENING") {
    BirdPtr bird = awake_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LOADING, 0.0);
    const Frame loading = render(bird.get(), 0.25);
    CHECK(loading.moving == 0); // nothing to play: awake already
    CHECK(wing(loading) < 0.2);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.5);
    CHECK(render(bird.get(), 0.75).moving == 1);
    const Frame frame = render(bird.get(), 1.1);
    CHECK(frame.moving == 0);
    CHECK(wing(frame) > 0.5);
}

TEST_CASE("dictation ending lowers the wing; transcribing and idle are both the resting pose") {
    BirdPtr bird = awake_bird();
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
    BirdPtr bird = awake_bird();
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
    BirdPtr bird = awake_bird();
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

TEST_CASE("reduced motion: changes jump to their end pose, through idle if they must") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_reduced_motion(bird.get(), 1);
    // Asleep to listening in one go: both hops, no frames between.
    mynah_bird_set_model(bird.get(), MYNAH_MODEL_LOADED, 0.0);
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
    BirdPtr bird = awake_bird();
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

// --- the pill: the bird in its circle, the voice as its rim pulsing --------

namespace {

constexpr int kSide = 128;
constexpr double kRadius = kSide * MYNAH_PILL_CIRCLE / 2.0;

Frame pill(mynah_bird* bird, double now) {
    Frame frame;
    frame.argb.assign(std::size_t(kSide) * kSide, 0xdeadbeef);
    frame.moving = mynah_bird_render_pill(bird, now, frame.argb.data(), kSide, kSide);
    return frame;
}

// The painted fraction of the ring between `inner` and `outer` (multiples
// of the circle's radius) around the centre.
double annulus(const Frame& frame, double inner, double outer, uint32_t min_alpha = 96) {
    int painted = 0, total = 0;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x) {
            const double dx = x + 0.5 - kSide / 2.0, dy = y + 0.5 - kSide / 2.0;
            const double r = std::sqrt(dx * dx + dy * dy) / kRadius;
            if (r < inner || r >= outer) continue;
            ++total;
            if ((frame.argb[std::size_t(y) * kSide + x] >> 24) > min_alpha) ++painted;
        }
    return total ? double(painted) / total : 0.0;
}
// The rim the rings rest on, and the margin they swell into.
double rim(const Frame& frame) { return annulus(frame, 0.98, 1.10); }
double reach(const Frame& frame) { return annulus(frame, 1.16, 1.45); }

// The rings' colour: the mean of their solid pixels around the rim
// (premultiplied, so compare channels with each other, not with a tint).
struct Colour {
    double r = 0, g = 0, b = 0;
};
Colour ring_colour(const Frame& frame) {
    Colour sum;
    int n = 0;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x) {
            const double dx = x + 0.5 - kSide / 2.0, dy = y + 0.5 - kSide / 2.0;
            const double r = std::sqrt(dx * dx + dy * dy) / kRadius;
            const uint32_t px = frame.argb[std::size_t(y) * kSide + x];
            if (r < 1.0 || (px >> 24) < 200) continue;
            sum.r += (px >> 16) & 255;
            sum.g += (px >> 8) & 255;
            sum.b += px & 255;
            ++n;
        }
    if (n) sum = {sum.r / n, sum.g / n, sum.b / n};
    return sum;
}

// The bird in the middle of the circle.
double bird_in_circle(const Frame& frame) {
    int painted = 0, total = 0;
    for (int y = int(kSide * 0.42); y < int(kSide * 0.58); ++y)
        for (int x = int(kSide * 0.42); x < int(kSide * 0.58); ++x, ++total)
            if ((frame.argb[std::size_t(y) * kSide + x] >> 24) > 128) ++painted;
    return double(painted) / total;
}

void loud(mynah_bird* bird, double now, float low, float mid, float high) {
    float bands[MYNAH_SPECTRUM_BANDS] = {};
    for (int b = 0; b < 4; ++b) {
        bands[b] = low;
        bands[4 + b] = mid;
        bands[8 + b] = high;
    }
    mynah_bird_set_voice(bird, 0.9f, bands, now);
}

} // namespace

TEST_CASE("the pill: the bird in the circle, and while idle a still, round rim") {
    BirdPtr bird = awake_bird();
    REQUIRE(bird != nullptr);
    const Frame idle = pill(bird.get(), 0.0);
    CHECK(idle.moving == 0); // nothing turns while idle
    CHECK(bird_in_circle(idle) > 0.9);
    CHECK(rim(idle) > 0.3);    // the rings lie on the rim
    CHECK(reach(idle) < 0.01); // and nowhere else
}

TEST_CASE("listening, a loud voice swells the rim, and silence settles it again") {
    BirdPtr bird = awake_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    CHECK(reach(pill(bird.get(), 0.0)) < 0.02); // listening, no voice yet: calm
    for (double t = 0.0; t <= 0.5; t += 1.0 / 30) loud(bird.get(), t, 0.6f, 0.9f, 0.6f);
    const Frame swelled = pill(bird.get(), 0.5);
    CHECK(swelled.moving == 1);
    CHECK(reach(swelled) > 0.1);
    // No more voice: after kVoiceStale it counts as silence, and the rings
    // settle to the calm swing they keep while the engine listens.
    const Frame calm = pill(bird.get(), 2.0);
    CHECK(calm.moving == 1); // still turning: the engine still listens
    CHECK(reach(calm) < 0.02);
}

TEST_CASE("each ring follows its own band: the same loudness low or high draws different rims") {
    BirdPtr low = awake_bird(), high = awake_bird();
    REQUIRE(low != nullptr);
    REQUIRE(high != nullptr);
    for (mynah_bird* bird : {low.get(), high.get()}) {
        mynah_bird_set_state(bird, MYNAH_LISTENING, 0.0);
        pill(bird, 0.0);
    }
    loud(low.get(), 0.4, 0.9f, 0.0f, 0.0f);
    loud(high.get(), 0.4, 0.0f, 0.0f, 0.9f);
    const Frame a = pill(low.get(), 0.5), b = pill(high.get(), 0.5);
    CHECK(reach(a) > 0.03);
    CHECK(reach(b) > 0.03);
    CHECK(difference(a, b) > 0.01); // the same clock and loudness: only the bands differ
}

TEST_CASE("without bands, the level drives all three rings") {
    BirdPtr bird = awake_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    pill(bird.get(), 0.0);
    mynah_bird_set_voice(bird.get(), 0.9f, nullptr, 0.4);
    CHECK(reach(pill(bird.get(), 0.5)) > 0.1);
}

TEST_CASE("the rings take the state's colour: cyan while listening, amber while transcribing") {
    BirdPtr bird = awake_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    pill(bird.get(), 0.0);
    loud(bird.get(), 0.9, 0.8f, 0.8f, 0.8f);
    const Colour listening = ring_colour(pill(bird.get(), 1.0));
    CHECK(listening.b > listening.r + 60);
    mynah_bird_set_state(bird.get(), MYNAH_TRANSCRIBING, 1.0);
    const Colour transcribing = ring_colour(pill(bird.get(), 2.0));
    CHECK(transcribing.r > transcribing.b + 60);
    // Back to idle: grey, and the turning stops once everything has settled.
    mynah_bird_set_state(bird.get(), MYNAH_IDLE, 2.0);
    pill(bird.get(), 3.0);
    CHECK(pill(bird.get(), 4.0).moving == 0);
}

TEST_CASE("the first pill drawn shows the voice as it is, with nothing to animate from") {
    BirdPtr bird = awake_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    loud(bird.get(), 0.5, 0.6f, 0.9f, 0.6f);
    CHECK(reach(pill(bird.get(), 0.5)) > 0.1);
}

TEST_CASE("the rings turn while listening, even in silence") {
    BirdPtr bird = awake_bird();
    REQUIRE(bird != nullptr);
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    // Slow on purpose — the slowest ring turns once in 8 s, and in silence
    // the rim moves about a pixel — so compare a second apart, not a frame.
    const Frame a = pill(bird.get(), 1.0);
    const Frame b = pill(bird.get(), 2.0);
    CHECK(a.moving == 1);
    CHECK(difference(a, b) > 0.0);
}

TEST_CASE("bad arguments are refused, not crashed on") {
    BirdPtr bird = make_bird();
    REQUIRE(bird != nullptr);
    uint32_t pixel = 0;
    CHECK(mynah_bird_render(nullptr, 0.0, &pixel, 1, 1) == -1);
    CHECK(mynah_bird_render(bird.get(), 0.0, nullptr, 1, 1) == -1);
    CHECK(mynah_bird_render(bird.get(), 0.0, &pixel, 0, 1) == -1);
    mynah_bird_set_state(nullptr, MYNAH_LISTENING, 0.0);
    mynah_bird_set_model(nullptr, MYNAH_MODEL_LOADED, 0.0);
    mynah_bird_set_voice(nullptr, 1.0f, nullptr, 0.0);
    CHECK(mynah_bird_render_pill(nullptr, 0.0, &pixel, 1, 1) == -1);
    // A broken voice is silence, not a broken picture.
    const float nan = std::nanf("");
    const float bands[MYNAH_SPECTRUM_BANDS] = {nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan};
    mynah_bird_set_state(bird.get(), MYNAH_LISTENING, 0.0);
    mynah_bird_set_voice(bird.get(), nan, bands, 0.0);
    std::vector<uint32_t> square(std::size_t(kSide) * kSide);
    CHECK(mynah_bird_render_pill(bird.get(), 0.5, square.data(), kSide, kSide) == 1);
    mynah_bird_set_reduced_motion(nullptr, 1);
    mynah_bird_destroy(nullptr);
}
