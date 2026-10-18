// mynah_bird_* — the pill's animated bird, rendered by ThorVG from the
// Lottie file compiled into the library (assets/bird/bird.json; see
// docs/LINUX-KDE-PLAN.md, "Animated bird").
//
// The file's markers are its segments: "idle-to-listening" plays when
// dictation starts, "listening-to-idle" when it ends; each transition's
// last frame is the pose the bird then rests in, and the first frame of the
// other. A change that arrives mid-transition plays that transition
// backwards from the frame it is on — the exact pose, so no jump — rather
// than starting the other one. All of this is decided here and nowhere else — the pills only pass the engine's state and a clock, and
// show the premultiplied ARGB this renders (what Qt's
// Format_ARGB32_Premultiplied and CoreGraphics' premultiplied-first,
// 32-bit little-endian layout both take as-is). So the two cannot drift.
//
// Not thread-safe: ThorVG is built without its thread support
// (third_party/thorvg.cmake), so every bird in the process belongs to one
// thread. Rendering a ~64 px bird takes microseconds; there is nothing to
// move off the UI thread.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <thorvg.h>
#include <thorvg_lottie.h>

#include "mynah/mynah.h"

// Generated at build time from assets/bird/bird.json (core/CMakeLists.txt).
extern const unsigned char mynah_bird_json[];
extern const unsigned int mynah_bird_json_size;

namespace {

constexpr const char* kToListening = "idle-to-listening";
constexpr const char* kToIdle = "listening-to-idle";

// A marker in the Lottie file, in the file's frames.
struct Segment {
    std::string name;
    float begin = 0.0f, end = 0.0f;
};

} // namespace

struct mynah_bird {
    tvg::SwCanvas* canvas = nullptr;
    tvg::LottieAnimation* animation = nullptr;
    float fps = 60.0f;
    float frames = 0.0f; // the whole file
    float native_width = 0.0f, native_height = 0.0f;
    Segment to_listening, to_idle;

    // What is playing: frames `from` → `to` of `segment` (counted from the
    // segment's start), set off at `started` on the caller's clock; at rest
    // once it reaches `to`. Before any state arrives the bird rests at the
    // end of the stop transition — the idle pose (create() sets it).
    bool listening = false;
    const Segment* segment = nullptr;
    float from = 0.0f, to = 0.0f;
    double started = 0.0;
    bool reduced_motion = false;
    const Segment* selected = nullptr; // what ThorVG has selected

    std::vector<uint32_t> target; // the canvas's buffer, re-made per render size
    int width = 0, height = 0;

    static float length(const Segment& segment) { return segment.end - segment.begin; }

    // Where the playing transition is at `now`, and whether it got there.
    float frame_at(double now, bool* resting) const {
        const double elapsed = now - started;
        const double moved = elapsed > 0 ? elapsed * fps : 0.0; // NaN lands on 0 too
        const double distance = std::fabs(double(to) - double(from));
        *resting = reduced_motion || moved >= distance;
        if (*resting) return to;
        return float(to > from ? from + moved : from - moved);
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
    bool found_listening = false, found_idle = false;
    for (uint32_t i = 0; i < bird->animation->markersCnt(); ++i) {
        Segment segment;
        const char* name = bird->animation->marker(i, &segment.begin, &segment.end);
        if (!name) continue;
        segment.name = name;
        if (segment.name == kToListening) {
            bird->to_listening = segment;
            found_listening = true;
        } else if (segment.name == kToIdle) {
            bird->to_idle = segment;
            found_idle = true;
        }
    }
    // A re-export that lost a marker fails here, loudly, rather than as a
    // bird that never moves.
    if (bird->fps <= 0 || bird->native_width <= 0 || bird->native_height <= 0 ||
        !found_listening || !found_idle ||
        bird->to_listening.end <= bird->to_listening.begin || bird->to_idle.end <= bird->to_idle.begin) {
        mynah_bird_destroy(bird);
        return nullptr;
    }
    bird->segment = &bird->to_idle;
    bird->from = bird->to = mynah_bird::length(bird->to_idle);
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
    const bool listening = state == MYNAH_LOADING || state == MYNAH_LISTENING;
    if (listening == bird->listening) return;
    bird->listening = listening;
    bool resting = false;
    const float frame = bird->frame_at(now, &resting);
    if (resting) {
        // The designed transition, from its first frame — the pose the bird
        // is resting in, whichever way it got there.
        bird->segment = listening ? &bird->to_listening : &bird->to_idle;
        bird->from = 0.0f;
        bird->to = mynah_bird::length(*bird->segment);
    } else {
        // Mid-transition: turn back from this very frame. The start
        // transition ends listening and the stop one idle; their first
        // frames are the other pose.
        const bool forward = (bird->segment == &bird->to_listening) == listening;
        bird->from = frame;
        bird->to = forward ? mynah_bird::length(*bird->segment) : 0.0f;
    }
    bird->started = now;
}

void mynah_bird_set_reduced_motion(mynah_bird *bird, int reduced) {
    if (bird) bird->reduced_motion = reduced != 0;
}

int mynah_bird_render(mynah_bird *bird, double now, uint32_t *argb, int width, int height) {
    if (!bird || !argb || width <= 0 || height <= 0) return -1;

    const Segment& segment = *bird->segment;
    if (bird->selected != &segment) {
        if (bird->animation->segment(segment.name.c_str()) != tvg::Result::Success) return -1;
        bird->selected = &segment;
    }
    bool resting = false;
    const float frame = bird->frame_at(now, &resting);
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
    return resting ? 0 : 1;
}

} // extern "C"
