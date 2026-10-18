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
// All of this is decided here and nowhere else: the pills only pass the
// engine's state and model status and a clock, and show the premultiplied
// ARGB this renders (what Qt's Format_ARGB32_Premultiplied and
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
    // The animation and the canvas each hold a reference to the picture.
    if (bird->canvas->add(picture) != tvg::Result::Success) {
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

void mynah_bird_set_reduced_motion(mynah_bird *bird, int reduced) {
    if (bird) bird->reduced_motion = reduced != 0;
}

int mynah_bird_render(mynah_bird *bird, double now, uint32_t *argb, int width, int height) {
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

    if (width != bird->width || height != bird->height) {
        bird->target.assign(std::size_t(width) * std::size_t(height), 0);
        if (bird->canvas->target(bird->target.data(), uint32_t(width), uint32_t(width),
                                 uint32_t(height), tvg::ColorSpace::ARGB8888) != tvg::Result::Success) {
            bird->width = bird->height = 0;
            return -1;
        }
        bird->width = width;
        bird->height = height;
        // Fit the file's square into whatever the caller gave, centred,
        // keeping the aspect.
        const float scale = std::min(float(width) / bird->native_width, float(height) / bird->native_height);
        tvg::Picture* picture = bird->animation->picture();
        picture->size(bird->native_width * scale, bird->native_height * scale);
        picture->translate((float(width) - bird->native_width * scale) / 2,
                           (float(height) - bird->native_height * scale) / 2);
    }
    if (bird->canvas->update() != tvg::Result::Success) return -1;
    if (bird->canvas->draw(/*clear=*/true) != tvg::Result::Success) return -1;
    if (bird->canvas->sync() != tvg::Result::Success) return -1;
    std::memcpy(argb, bird->target.data(), bird->target.size() * sizeof(uint32_t));
    return moving ? 1 : 0;
}

} // extern "C"
