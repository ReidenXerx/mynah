#!/usr/bin/env bash
# Build whisper.cpp from the vendored source.
#
# Replaces linking Homebrew's dylibs, which made the app unshippable for three
# reasons: it hardcoded /opt/homebrew paths (breaking on Intel Macs and any
# machine without whisper-cpp installed), Homebrew's binaries are built for a
# far newer macOS than our 13.0 target, and ggml's compute backends were
# separate .so files living in Cellar/*/libexec that the app loaded at runtime.
#
# Two build options do the heavy lifting:
#
#   BUILD_SHARED_LIBS=OFF        Static libraries. With static ggml the compute
#                                backends are compiled in and registered
#                                directly, so there are no loadable modules to
#                                find, no dylibs to copy into the bundle, and no
#                                install_name_tool rewriting.
#
#   GGML_METAL_EMBED_LIBRARY=ON  Compiles the Metal shaders into the binary.
#                                Otherwise ggml looks for ggml-metal.metal on
#                                disk at runtime and silently drops to CPU when
#                                it is missing.
#
#   GGML_NATIVE=OFF              Required, for two independent reasons.
#
#                                Correctness: a native build bakes in the CPU
#                                features of whichever machine compiled it. An
#                                app built on an M4 can then fault on an older
#                                Mac. Since the point of vendoring is a binary
#                                we can hand to someone else, tuning it to this
#                                machine defeats the exercise.
#
#                                Practicality: ggml's native detection probes
#                                CPU features by compiling *and running* small
#                                test programs, and its SVE probe hangs forever
#                                on Apple Silicon — the binary spins at 100% CPU
#                                instead of faulting, so cmake waits on it
#                                indefinitely. Configure never returns.
#
# Together those turn "bundle a tree of libraries and hope the paths resolve"
# into "link one archive".
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/vendor/whisper.cpp"
BUILD="$ROOT/vendor/build"
PREFIX="$ROOT/vendor/install"

# Must match Package.swift and build-app.sh.
DEPLOYMENT_TARGET="13.0"

if [ ! -f "$SRC/CMakeLists.txt" ]; then
  echo "error: vendored whisper.cpp missing. Run:" >&2
  echo "  git submodule update --init --recursive" >&2
  exit 1
fi

# Skip the (slow) rebuild when the install tree is newer than the source.
if [ -f "$PREFIX/lib/libwhisper.a" ] && [ "$PREFIX/lib/libwhisper.a" -nt "$SRC/CMakeLists.txt" ]; then
  echo "whisper.cpp already built at $PREFIX (delete vendor/install to force)"
  exit 0
fi

echo "configuring whisper.cpp (static, Metal, deployment target $DEPLOYMENT_TARGET)…"
cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_METAL=ON \
  -DGGML_METAL_EMBED_LIBRARY=ON \
  -DGGML_ACCELERATE=ON \
  -DGGML_BLAS=OFF \
  -DGGML_NATIVE=OFF \
  -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16 \
  -DWHISPER_BUILD_TESTS=OFF \
  -DWHISPER_BUILD_EXAMPLES=OFF \
  -DWHISPER_BUILD_SERVER=OFF

echo "building…"
cmake --build "$BUILD" --config Release -j "$(sysctl -n hw.ncpu)"
cmake --install "$BUILD" > /dev/null

echo "installed to $PREFIX"
find "$PREFIX/lib" -name '*.a' | sed 's|.*/|  |'
