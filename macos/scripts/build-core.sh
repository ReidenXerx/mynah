#!/usr/bin/env bash
# Build the C++ core and the pinned whisper.cpp, and install them into
# vendor/install — where the CMynah module map and Package.swift's link line
# point (Phase 5: the app runs on the core, so this replaces
# build-whisper.sh, which built only whisper.cpp).
#
# One CMake invocation builds everything: the root project already carries
# the Apple flags that make the app shippable — Metal compiled in
# (GGML_METAL_EMBED_LIBRARY), GGML_NATIVE=OFF for the same two reasons the
# old whisper script documented (CPU features baked in; the SVE probe
# hanging configure) — and the deployment target is 13.0 from the root.
#
# The Linux front end and the tests are skipped: this is the app's build.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
BUILD="$ROOT/vendor/core-build"
PREFIX="$ROOT/vendor/install"

# Must match Package.swift and build-app.sh.
DEPLOYMENT_TARGET="13.0"

if [ ! -f "$REPO/third_party/whisper.cpp/CMakeLists.txt" ]; then
  echo "error: the whisper.cpp submodule is missing. Run:" >&2
  echo "  git submodule update --init --recursive" >&2
  exit 1
fi

# Skip the (slow) rebuild only when nothing that goes into the install tree
# has changed. Everything the library is built FROM has to be in this list:
# a stale install is the worst kind of build failure, because it looks like
# a pass. (The sources and headers were checked from the start; the build
# files and the pinned whisper.cpp were not — and a submodule bump answered
# by yesterday's libwhisper is exactly the silent version skew M6 exists to
# prevent.)
WHISPER_COMMIT="$(git -C "$REPO/third_party/whisper.cpp" rev-parse HEAD 2>/dev/null || echo unknown)"
STAMP="$PREFIX/.built-from"
if [ -f "$PREFIX/lib/libmynah.a" ] &&
   [ -z "$(find "$REPO/core/include" "$REPO/core/src" "$REPO/core/CMakeLists.txt" \
            "$REPO/CMakeLists.txt" "$ROOT/scripts/build-core.sh" \
            -newer "$PREFIX/lib/libmynah.a" 2>/dev/null)" ] &&
   [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$WHISPER_COMMIT" ]; then
  echo "core already built at $PREFIX (delete vendor/install to force)"
  exit 0
fi

echo "configuring the core (static, Metal, deployment target $DEPLOYMENT_TARGET)…"
cmake -S "$REPO" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DMYNAH_BUILD_TESTS=OFF \
  -DMYNAH_BUILD_LINUX=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_NATIVE=OFF \
  -DGGML_METAL=ON \
  -DGGML_METAL_EMBED_LIBRARY=ON \
  -DGGML_ACCELERATE=ON \
  -DGGML_BLAS=OFF \
  -DGGML_CPU_ARM_ARCH="armv8.2-a+dotprod+fp16" \
  -DWHISPER_BUILD_TESTS=OFF \
  -DWHISPER_BUILD_EXAMPLES=OFF \
  -DWHISPER_BUILD_SERVER=OFF

echo "building…"
cmake --build "$BUILD" --config Release -j "$(sysctl -n hw.ncpu)"
cmake --install "$BUILD" > /dev/null

# What this install was built from, for the freshness check above.
printf '%s' "$WHISPER_COMMIT" > "$STAMP"

echo "installed to $PREFIX"
find "$PREFIX/lib" -name '*.a' | sed 's|.*/|  |'