// PipeWire capture. Linux only, and the live cases only where a session
// PipeWire is running: they exist to put start/stop — the listener's
// lifetime, the teardown order — under ASan and TSan, which is where the
// stack-allocated events table was a use-after-free.

#include "vendor/doctest.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "env.hpp"
#include "mynah/mynah.h"
#include "pipewire_capture.hpp"

namespace {

bool pipewire_running() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime != nullptr && std::filesystem::exists(std::filesystem::path(runtime) / "pipewire-0");
}

struct Engine {
    mynah_engine* engine = nullptr;
    Engine() {
        char* error = nullptr;
        engine = mynah_create(nullptr, [](const mynah_event*, void*) {}, nullptr, &error);
        std::free(error);
    }
    ~Engine() { mynah_destroy(engine); }
};

} // namespace

TEST_CASE("no PipeWire: start fails with a reason, and leaves nothing behind") {
    mynah_test::TmpDir dir;
    // A remote that does not exist: the connect is refused.
    mynah_test::EnvOverride remote("PIPEWIRE_REMOTE", (dir.path() / "no-such-socket").string());
    Engine engine;
    REQUIRE(engine.engine != nullptr);

    mynah::capture::PipeWireCapture capture(engine.engine);
    CHECK_FALSE(capture.start());
    CHECK_FALSE(capture.error().empty());
    // Again, after the failure cleaned up: no double free, no leaked loop.
    CHECK_FALSE(capture.start());
    capture.stop();
    capture.stop();
}

TEST_CASE("a live stream starts, stops, and starts again") {
    if (!pipewire_running()) {
        MESSAGE("no session PipeWire here; skipped");
        return;
    }
    Engine engine;
    REQUIRE(engine.engine != nullptr);

    mynah::capture::PipeWireCapture capture(engine.engine);
    for (int round = 0; round < 2; ++round) {
        REQUIRE_MESSAGE(capture.start(), capture.error());
        // Long enough for negotiation and a few process callbacks.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        capture.stop();
    }
}
