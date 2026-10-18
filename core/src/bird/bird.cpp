// mynah_bird_* — the pill's animated bird, rendered by ThorVG from the
// Lottie file compiled into the library (assets/bird/bird.json; see
// docs/LINUX-KDE-PLAN.md, "Animated bird").
//
// The bird rests in one of three poses, in a line — listening, idle,
// sleeping — and the file's markers are the four transitions between
// neighbours: "idle-to-listening" and "listening-to-idle" for dictation,
// "idle-to-sleeping" and "sleeping-to-idle" for the model (and with it the
// GPU) being let go and woken. Each transition's last frame is the pose the
// bird then rests in, and the first frame of the way back. Getting from one
// end of the line to the other plays both hops, one after the other. A
// change that arrives mid-transition plays that transition backwards from
// the frame it is on — the exact pose, so no jump — when that is the
// shorter way to where the bird now has to be.
//
// mynah_bird_render_pill also draws the voice: the rim of the bird's circle
// pulses with it — three wavy rings around the circle, one each for the
// low, middle and high bands of the spectrum the engine sends, swelling
// out and turning slowly, in the state's tint.
//
// All of this is decided here and nowhere else: the pills only pass the
// engine's state, model status and voice and a clock, and show the
// premultiplied ARGB this renders (what Qt's Format_ARGB32_Premultiplied and
// CoreGraphics' premultiplied-first, 32-bit little-endian layout both take
// as-is). So the two cannot drift.
//
// Not thread-safe: ThorVG is built without its thread support
// (third_party/thorvg.cmake), so every bird in the process belongs to one
// thread. Rendering a ~64 px bird takes microseconds; there is nothing to
// move off the UI thread.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <thorvg.h>
#include <thorvg_lottie.h>

#include "mynah/mynah.h"

// Generated at build time from assets/bird/bird.json (core/CMakeLists.txt).
extern const unsigned char mynah_bird_json[];
extern const unsigned int mynah_bird_json_size;

namespace {

// The poses, in their line: a transition only ever joins neighbours.
enum class Pose { Listening = 0, Idle = 1, Sleeping = 2 };

int distance(Pose a, Pose b) { return std::abs(int(a) - int(b)); }

// A marker in the Lottie file, in the file's frames, and the poses it joins.
struct Segment {
    const char* name;
    Pose from, to;
    float begin = 0.0f, end = 0.0f;
    bool found = false;
    float length() const { return end - begin; }
};

enum { ToListening, ToIdle, ToSleeping, ToAwake, SegmentCount };

constexpr float kPi = 3.14159265f;

// The state tints (macos/Sources/MynahApp/Session/DictationState.swift):
// the colour of the waves, as of the bars before them.
struct Rgb {
    float r, g, b;
};

Rgb tint(mynah_state state) {
    switch (state) {
    case MYNAH_LOADING: return {0.45f, 0.55f, 0.75f};
    case MYNAH_LISTENING: return {0.20f, 0.80f, 0.95f};
    case MYNAH_TRANSCRIBING: return {0.95f, 0.70f, 0.20f};
    case MYNAH_IDLE: break;
    }
    return {0.60f, 0.60f, 0.65f};
}

// The pill's picture is square: the bird's circle in the middle (its
// diameter MYNAH_PILL_CIRCLE of the side), the bird's box a little inside
// it, and around the rim the rings — at rest just outside it, swelling out
// by up to kRingReach of the radius. kHalo is the dark edge under each
// ring, so it reads on light windows. Widths are fractions of the diameter.
constexpr float kBirdInCircle = 0.9f;
constexpr float kRingRest = 1.03f;
constexpr float kRingReach = 0.30f;
constexpr float kHalo = 0.035f;

// One ring per band group (bands 0-3, 4-7, 8-11, low to high). The gains
// bring each group's speech level near full swing: measured on
// whisper.cpp's jfk.wav, the 90th percentile of the group means is 0.54,
// 0.89 and 0.57 — the middle carries speech, the ends less. Each ring has
// its own number of lobes and turns at its own speed (turns a second of
// flow; the middle one the other way); the speeds' periods all divide
// kFlowPeriod, so the flow can wrap there without a jump.
struct RingStyle {
    int first_band;
    float gain;
    int lobes;
    float turns;
    float phase;
    float width;
    float alpha;
};
constexpr RingStyle kRings[3] = {
    {0, 1.6f, 3, 0.125f, 0.0f, 0.036f, 0.95f},
    {4, 1.0f, 5, -0.2f, 1.3f, 0.027f, 0.62f},
    {8, 1.6f, 7, 0.25f, 2.6f, 0.020f, 0.42f},
};
constexpr double kFlowPeriod = 40.0;

// How the rings move, by state: how fast they turn (times their own
// speeds) and the calm swing they keep with no voice — about a pixel, so a
// live microphone shows as a gently moving rim even in silence (0.07 was
// measured invisible: under half a pixel). Idle is a still, round rim.
float flow_speed(mynah_state state) {
    switch (state) {
    case MYNAH_LISTENING: return 1.0f;
    case MYNAH_LOADING:
    case MYNAH_TRANSCRIBING: return 0.4f;
    case MYNAH_IDLE: break;
    }
    return 0.0f;
}
float calm_swing(mynah_state state) { return state == MYNAH_IDLE ? 0.0f : 0.15f; }

// Rising with the voice quickly, settling more slowly; a voice that stops
// arriving counts as silence after kVoiceStale.
constexpr double kRise = 0.06, kFall = 0.28, kTintChange = 0.12;
constexpr double kVoiceStale = 0.25;

float approach(float value, float target, double dt, double tau) {
    return value + (target - value) * float(1.0 - std::exp(-dt / tau));
}

uint8_t to8(float unit) { return uint8_t(std::clamp(unit, 0.0f, 1.0f) * 255.0f + 0.5f); }

} // namespace

struct mynah_bird {
    tvg::SwCanvas* canvas = nullptr;
    tvg::LottieAnimation* animation = nullptr;
    float fps = 60.0f;
    float frames = 0.0f; // the whole file
    float native_width = 0.0f, native_height = 0.0f;
    Segment segments[SegmentCount] = {
        {"idle-to-listening", Pose::Idle, Pose::Listening},
        {"listening-to-idle", Pose::Listening, Pose::Idle},
        {"idle-to-sleeping", Pose::Idle, Pose::Sleeping},
        {"sleeping-to-idle", Pose::Sleeping, Pose::Idle},
    };

    // What the front end last said. The model starts unloaded — nothing
    // loads before the first dictation — so the bird starts asleep.
    mynah_state state = MYNAH_IDLE;
    bool model_unloaded = true;
    bool reduced_motion = false;

    // What is playing: frames `from` → `to` of `segment` (counted from its
    // start; `to` is always 0 or its length), set off at `started` on the
    // caller's clock, at rest once it reaches `to`. create() sets it at
    // rest at the end of falling asleep.
    const Segment* segment = nullptr;
    float from = 0.0f, to = 0.0f;
    double started = 0.0;
    const Segment* selected = nullptr; // what ThorVG has selected

    std::vector<uint32_t> target; // the canvas's buffer, re-made per render size
    int width = 0, height = 0;
    bool pill_layout = false;     // what the picture is laid out for

    // The voice and its rings: the band groups' levels from the last
    // set_voice and when it came, the rings' smoothed swings (0..1), their
    // colour and how long they have flowed (seconds at full speed, wrapped
    // at kFlowPeriod), and the last pill render's clock, for the steps
    // between.
    float voice[3] = {0, 0, 0};
    double voice_at = -1e9;
    float swing[3] = {0, 0, 0};
    Rgb colour = tint(MYNAH_IDLE);
    double flowed = 0.0;
    double ticked = std::nan("");
    tvg::Scene* waves = nullptr; // in the canvas, under the bird

    // Where the bird has to be: listening only while the engine listens —
    // not while it loads, which is waking up — and asleep while idle with
    // the model unloaded.
    Pose goal() const {
        if (state == MYNAH_LISTENING) return Pose::Listening;
        if (state == MYNAH_IDLE && model_unloaded) return Pose::Sleeping;
        return Pose::Idle;
    }

    // The pose the playing transition is headed for, and the one it left.
    Pose heading() const { return to > 0 ? segment->to : segment->from; }
    Pose leaving() const { return to > 0 ? segment->from : segment->to; }

    // Where the playing transition is at `now`, and whether it got there.
    float frame_at(double now, bool* resting) const {
        const double elapsed = now - started;
        const double moved = elapsed > 0 ? elapsed * fps : 0.0; // NaN lands on 0 too
        const double way = std::fabs(double(to) - double(from));
        *resting = reduced_motion || moved >= way;
        if (*resting) return to;
        return float(to > from ? from + moved : from - moved);
    }

    // The designed transition out of `at`, toward `where`.
    const Segment* hop(Pose at, Pose where) const {
        if (at == Pose::Listening) return &segments[ToIdle];
        if (at == Pose::Sleeping) return &segments[ToAwake];
        return where == Pose::Listening ? &segments[ToListening] : &segments[ToSleeping];
    }

    void play(const Segment* next, double at) {
        segment = next;
        from = 0.0f;
        to = next->length();
        started = at;
    }

    // Bring the motion up to `now` under the current goal: a transition that
    // reached its pose while the bird still has further to go hands over to
    // the next hop from the moment it ended, so a late render lands where a
    // punctual one would have. Returns the frame to draw.
    float advance(double now, bool* moving) {
        for (int hops = 0;; ++hops) {
            bool resting = false;
            const float frame = frame_at(now, &resting);
            const Pose where = goal();
            if (!resting || heading() == where || hops == 2) {
                *moving = !resting || heading() != where;
                return frame;
            }
            const double ended = reduced_motion ? now : started + std::fabs(double(to) - double(from)) / fps;
            play(hop(heading(), where), ended);
        }
    }

    // Move the rings on to `now`: each swing toward its band's voice (or the
    // state's calm swing, if more), the colour toward the state's tint, the
    // turning along at the state's speed. Returns whether anything still
    // moves.
    bool tick_waves(double now) {
        // However long since the last render: smoothing converges, and the
        // flow is periodic in one cycle, so a gap just lands where it would.
        // The first render has nothing to animate from: it starts settled.
        const bool first = std::isnan(ticked);
        const double dt = first ? 0.0 : std::max(0.0, now - ticked);
        const double settle = first ? HUGE_VAL : dt;
        ticked = now;
        const bool heard = (state == MYNAH_LISTENING || state == MYNAH_LOADING) && now - voice_at < kVoiceStale;
        bool moving = false;
        for (int k = 0; k < 3; ++k) {
            const float target = std::max(calm_swing(state), heard ? voice[k] : 0.0f);
            swing[k] = approach(swing[k], target, settle, target > swing[k] ? kRise : kFall);
            moving = moving || std::fabs(target - swing[k]) > 0.002f;
        }
        const Rgb want = tint(state);
        colour = {approach(colour.r, want.r, settle, kTintChange), approach(colour.g, want.g, settle, kTintChange),
                  approach(colour.b, want.b, settle, kTintChange)};
        moving = moving || std::fabs(colour.r - want.r) + std::fabs(colour.g - want.g) +
                                   std::fabs(colour.b - want.b) > 0.01f;
        const float speed = flow_speed(state);
        flowed = std::fmod(flowed + speed * dt, kFlowPeriod); // kept small: it goes into float sin()
        return moving || speed > 0.0f;
    }

    // The rings as ThorVG strokes, around the circle of a `width`×`height`
    // pill: every ring's dark halo first, then the colours, the faint high
    // ring at the back. Each swells only outward — at rest a thin rim, and
    // with the voice its lobes bulge out by up to kRingReach of the radius.
    void draw_rings(int width, int height) {
        waves->remove();
        const float side = float(std::min(width, height));
        const float diameter = side * MYNAH_PILL_CIRCLE, radius = diameter / 2.0f;
        const float cx = float(width) / 2.0f, cy = float(height) / 2.0f;
        constexpr int kPoints = 144;
        for (int pass = 0; pass < 2; ++pass) {
            for (int k = 2; k >= 0; --k) {
                const RingStyle& style = kRings[k];
                const float turned = float(2.0 * kPi * std::fmod(style.turns * flowed, 1.0));
                const float reach = radius * kRingReach * swing[k];
                tvg::Shape* shape = tvg::Shape::gen();
                for (int i = 0; i < kPoints; ++i) {
                    const float theta = 2.0f * kPi * float(i) / kPoints;
                    const float r = radius * kRingRest +
                                    reach * (0.55f + 0.45f * std::sin(float(style.lobes) * theta + turned + style.phase));
                    const float x = cx + r * std::cos(theta), y = cy + r * std::sin(theta);
                    if (i == 0) shape->moveTo(x, y);
                    else shape->lineTo(x, y);
                }
                shape->close();
                shape->strokeJoin(tvg::StrokeJoin::Round);
                if (pass == 0) {
                    shape->strokeWidth(diameter * (style.width + kHalo));
                    shape->strokeFill(0, 0, 0, to8(0.22f * style.alpha));
                } else {
                    shape->strokeWidth(diameter * style.width);
                    shape->strokeFill(to8(colour.r), to8(colour.g), to8(colour.b), to8(style.alpha));
                }
                waves->add(shape);
            }
        }
    }

    // The goal just changed, at `now`: set off toward it from wherever the
    // bird is — the next hop if it is at rest, or back the way it came if
    // that is the shorter way.
    void retarget(double now) {
        bool resting = false;
        const float frame = frame_at(now, &resting);
        const Pose where = goal();
        if (resting) {
            if (heading() != where) play(hop(heading(), where), now);
            return;
        }
        if (distance(heading(), where) > distance(leaving(), where)) {
            from = frame;
            to = to > 0 ? 0.0f : segment->length();
            started = now;
        }
    }
};

namespace {

// What mynah_bird_render and mynah_bird_render_pill share; `pill` puts the
// bird in the circle and draws the rings around it.
int render(mynah_bird* bird, double now, uint32_t* argb, int width, int height, bool pill) {
    if (!bird || !argb || width <= 0 || height <= 0) return -1;

    bool moving = false;
    const float frame = bird->advance(now, &moving);
    const Segment& segment = *bird->segment;
    if (bird->selected != &segment) {
        if (bird->animation->segment(segment.name) != tvg::Result::Success) return -1;
        bird->selected = &segment;
    }
    // ThorVG counts frames from the selected segment's start, not the
    // file's — and the file's own last frame number draws nothing (Lottie
    // layers show for in <= frame < out), so a segment ending there holds
    // just short of it.
    const float last = std::min(segment.end, bird->frames - 0.001f) - segment.begin;
    bird->animation->frame(std::clamp(frame, 0.0f, std::max(0.0f, last)));
    // (frame() reports InsufficientCondition for the frame already shown: not an error.)

    if (width != bird->width || height != bird->height || pill != bird->pill_layout) {
        if (width != bird->width || height != bird->height) {
            bird->target.assign(std::size_t(width) * std::size_t(height), 0);
            if (bird->canvas->target(bird->target.data(), uint32_t(width), uint32_t(width),
                                     uint32_t(height), tvg::ColorSpace::ARGB8888) != tvg::Result::Success) {
                bird->width = bird->height = 0;
                return -1;
            }
        }
        bird->width = width;
        bird->height = height;
        bird->pill_layout = pill;
        // The file's square, kept square and centred: fitted to the whole
        // canvas, or for the pill a little inside the circle.
        const float box = pill ? float(std::min(width, height)) * MYNAH_PILL_CIRCLE * kBirdInCircle
                               : float(std::min(width, height));
        const float fit = std::min(box / bird->native_width, box / bird->native_height);
        tvg::Picture* picture = bird->animation->picture();
        picture->size(bird->native_width * fit, bird->native_height * fit);
        picture->translate((float(width) - bird->native_width * fit) / 2,
                           (float(height) - bird->native_height * fit) / 2);
    }
    if (pill) {
        moving = bird->tick_waves(now) || moving;
        bird->draw_rings(width, height);
    } else {
        bird->waves->remove();
    }
    if (bird->canvas->update() != tvg::Result::Success) return -1;
    if (bird->canvas->draw(/*clear=*/true) != tvg::Result::Success) return -1;
    if (bird->canvas->sync() != tvg::Result::Success) return -1;
    std::memcpy(argb, bird->target.data(), bird->target.size() * sizeof(uint32_t));
    return moving ? 1 : 0;
}

} // namespace

extern "C" {

mynah_bird *mynah_bird_create(void) {
    // Initializer::init is reference-counted inside ThorVG; term() pairs it.
    if (tvg::Initializer::init(0) != tvg::Result::Success) return nullptr;
    auto* bird = new mynah_bird;
    bird->canvas = tvg::SwCanvas::gen();
    bird->animation = tvg::LottieAnimation::gen();
    if (!bird->canvas || !bird->animation) {
        mynah_bird_destroy(bird);
        return nullptr;
    }
    // copy=true is required, not a choice: the Lottie loader parses in situ
    // (rapidjson's kParseInsituFlag writes into the buffer) and needs the
    // NUL only its copy adds — the embedded array is const and unterminated.
    tvg::Picture* picture = bird->animation->picture();
    if (picture->load(reinterpret_cast<const char*>(mynah_bird_json), mynah_bird_json_size,
                      "lottie", nullptr, /*copy=*/true) != tvg::Result::Success) {
        mynah_bird_destroy(bird);
        return nullptr;
    }
    // No segment is selected yet, so these are the whole file's.
    bird->frames = bird->animation->totalFrame();
    const float seconds = bird->animation->duration();
    bird->fps = seconds > 0 ? bird->frames / seconds : 0.0f;
    picture->size(&bird->native_width, &bird->native_height);
    for (uint32_t i = 0; i < bird->animation->markersCnt(); ++i) {
        float begin = 0, end = 0;
        const char* name = bird->animation->marker(i, &begin, &end);
        if (!name) continue;
        for (Segment& segment : bird->segments) {
            if (std::strcmp(segment.name, name) != 0) continue;
            segment.begin = begin;
            segment.end = end;
            segment.found = true;
        }
    }
    // A re-export that lost a marker fails here, loudly, rather than as a
    // bird that never moves.
    bool complete = bird->fps > 0 && bird->native_width > 0 && bird->native_height > 0;
    for (const Segment& segment : bird->segments) complete = complete && segment.found && segment.length() > 0;
    if (!complete) {
        mynah_bird_destroy(bird);
        return nullptr;
    }
    bird->segment = &bird->segments[ToSleeping];
    bird->from = bird->to = bird->segment->length();
    // The rings first, so the bird draws over them; then the picture,
    // which the animation and the canvas each hold a reference to.
    bird->waves = tvg::Scene::gen();
    if (!bird->waves || bird->canvas->add(bird->waves) != tvg::Result::Success ||
        bird->canvas->add(picture) != tvg::Result::Success) {
        mynah_bird_destroy(bird);
        return nullptr;
    }
    return bird;
}

void mynah_bird_destroy(mynah_bird *bird) {
    if (!bird) return;
    delete bird->canvas;
    delete bird->animation;
    delete bird;
    tvg::Initializer::term();
}

void mynah_bird_set_state(mynah_bird *bird, mynah_state state, double now) {
    if (!bird) return;
    bool moving = false;
    bird->advance(now, &moving); // up to now under the old goal first
    if (state == bird->state) return;
    bird->state = state;
    bird->retarget(now);
}

void mynah_bird_set_model(mynah_bird *bird, mynah_model_status status, double now) {
    if (!bird) return;
    bool moving = false;
    bird->advance(now, &moving);
    const bool unloaded = status == MYNAH_MODEL_UNLOADED;
    if (unloaded == bird->model_unloaded) return;
    bird->model_unloaded = unloaded;
    bird->retarget(now);
}

void mynah_bird_set_voice(mynah_bird *bird, float level, const float *bands, double now) {
    if (!bird) return;
    for (int k = 0; k < 3; ++k) {
        float value = level;
        if (bands) {
            value = 0.0f;
            for (int b = 0; b < 4; ++b) value += bands[kRings[k].first_band + b];
            value = value / 4.0f * kRings[k].gain;
        }
        bird->voice[k] = value >= 0.0f ? std::min(value, 1.0f) : 0.0f; // NaN lands on 0 too
    }
    bird->voice_at = now;
}

void mynah_bird_set_reduced_motion(mynah_bird *bird, int reduced) {
    if (bird) bird->reduced_motion = reduced != 0;
}

int mynah_bird_render(mynah_bird *bird, double now, uint32_t *argb, int width, int height) {
    return render(bird, now, argb, width, height, /*pill=*/false);
}

int mynah_bird_render_pill(mynah_bird *bird, double now, uint32_t *argb, int width, int height) {
    return render(bird, now, argb, width, height, /*pill=*/true);
}

} // extern "C"
