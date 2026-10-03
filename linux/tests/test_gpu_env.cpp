// gpu_env — lifting a session's NVIDIA hiding for mynah alone. The pattern
// surgery is what can go wrong (dropping a pattern that hides something
// else, or missing the one that hides NVIDIA), so it is pinned here; apply()
// is checked against the real variable in a scratch environment.

#include "vendor/doctest.h"

#include <cstdlib>
#include <string>

#include "env.hpp"
#include "gpu_env.hpp"

using mynah::gpu_env::kNvidiaManifest;
using mynah::gpu_env::without_matching;

TEST_CASE("only the patterns that hide NVIDIA's driver are dropped") {
    // This machine's own setting (environment.d/90-gpu-default-igpu.conf).
    CHECK(without_matching("*nvidia*", kNvidiaManifest) == "");
    CHECK(without_matching("nvidia_icd.json", kNvidiaManifest) == "");
    CHECK(without_matching("*nvidia*,*lvp*", kNvidiaManifest) == "*lvp*");
    CHECK(without_matching("*lvp*, *nvidia* ,*radeon*", kNvidiaManifest) == "*lvp*,*radeon*");
    // Nothing about NVIDIA: untouched (normalised: trimmed, empties dropped).
    CHECK(without_matching("*radeon*,,*lvp*", kNvidiaManifest) == "*radeon*,*lvp*");
    CHECK(without_matching("", kNvidiaManifest) == "");
}

TEST_CASE("gpu on lifts the hiding for this process; gpu off leaves it") {
    // apply() keeps its verdict for the process, so one case covers both
    // orders: off first (nothing changes), then the variable a fresh start
    // would see with gpu on.
    mynah_test::EnvOverride hidden("VK_LOADER_DRIVERS_DISABLE", "*nvidia*,*lvp*");
    mynah::gpu_env::apply(false);
    CHECK(mynah::gpu_env::was_hidden());
    CHECK_FALSE(mynah::gpu_env::unhidden());
    CHECK(std::string(std::getenv("VK_LOADER_DRIVERS_DISABLE")) == "*nvidia*,*lvp*");

    mynah::gpu_env::apply(true);
    CHECK(mynah::gpu_env::unhidden());
    CHECK(std::string(std::getenv("VK_LOADER_DRIVERS_DISABLE")) == "*lvp*");
}
