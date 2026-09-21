# The C++ core and its dependencies (docs/ENGINE-MIGRATION.md, Phase 1).
#
# Layout: core/ (libmynah + tests), third_party/whisper.cpp (the pinned
# submodule). linux/ (headless CLI, KDE app) and the macOS front end join in
# later phases; the Swift app keeps its own SwiftPM build until Phase 5.
#
# The project version is the ONE version number (P9): mynah_version() reports
# it, and Info.plist / PKGBUILD take it from here.
cmake_minimum_required(VERSION 3.21)

project(mynah
    VERSION 0.14.0
    DESCRIPTION "One dictation engine, C++ behind a C API"
    LANGUAGES C CXX)

option(MYNAH_BUILD_TESTS "Build the libmynah test suite" ON)

if(MYNAH_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# whisper.cpp (M6): pinned at v1.9.2 in third_party/, statically linked into
# libmynah on every platform — the engine calls it in-process since Phase 2,
# so it is no longer optional. A bump is deliberate and re-runs the
# recognition test set. The options mirror macos/scripts/build-whisper.sh —
# the flags that make the macOS app shippable — so the core and the Swift
# app build the same source the same way until Phase 5 folds this file and
# that script into one build.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "static ggml/whisper" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
if(APPLE)
    # GGML_NATIVE=OFF twice over: a native build bakes the build machine's
    # CPU features into a binary meant for someone else's Mac, and ggml's
    # feature probes compile AND RUN test programs — its SVE probe spins at
    # 100% CPU forever on Apple Silicon, so configure never returns.
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    set(GGML_METAL ON CACHE BOOL "" FORCE)
    # Compile the Metal shaders in; otherwise ggml hunts for
    # ggml-metal.metal on disk at runtime and silently drops to CPU.
    set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
    set(GGML_ACCELERATE ON CACHE BOOL "" FORCE)
    set(GGML_BLAS OFF CACHE BOOL "" FORCE)
    set(GGML_CPU_ARM_ARCH "armv8.2-a+dotprod+fp16" CACHE STRING "" FORCE)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
    endif()
endif()
add_subdirectory(third_party/whisper.cpp)

# The Linux front end: only where it can build — its socket/injector layer
# is POSIX and compiles on the dev Mac; PipeWire capture and the `mynah`
# binary need libpipewire and stay Linux-only (see linux/CMakeLists.txt).
if(UNIX)
    add_subdirectory(linux)
endif()

add_subdirectory(core)