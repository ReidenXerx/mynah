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
option(MYNAH_BUILD_LINUX "Build the Linux front end (auto on Linux; off for an app-only build)" ON)

# Sanitized builds for the tests (docs/LINUX-KDE-PLAN.md, Stage 1): the
# engine is threaded, and ASan/TSan are how that stays honest. Applied to
# everything, whisper.cpp included, so TSan sees every access.
set(MYNAH_SANITIZE "" CACHE STRING "Build under a sanitizer: address (with UB), thread, or empty")
set_property(CACHE MYNAH_SANITIZE PROPERTY STRINGS "" address thread)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # Unset means -O0 for the core and whisper.cpp alike — a dev build that
    # transcribes at a fraction of real speed. The PKGBUILD passes Release.
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(MYNAH_BUILD_TESTS)
    enable_testing()
endif()

if(MYNAH_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(MYNAH_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
    # libgomp is not instrumented: under OpenMP every ggml worker handoff
    # reads as a race. ggml's own thread pool is plain atomics TSan can see.
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
elseif(NOT MYNAH_SANITIZE STREQUAL "")
    message(FATAL_ERROR "MYNAH_SANITIZE is '${MYNAH_SANITIZE}': use address, thread, or leave it empty")
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
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The CPU level the CPU backend is compiled for. The binary is static
    # (no GGML_BACKEND_DL, so no runtime pick between CPU variants), so this
    # is fixed at build time. x86-64-v3 — AVX2, FMA, F16C, BMI2: every Intel
    # Core since Haswell (2013), every AMD since Excavator (2015) — is what
    # the package ships; GGML_NATIVE=OFF alone would mean baseline x86-64,
    # no AVX at all, and the CPU tiers several times slower. `native` is for
    # a build that only ever runs on the machine that built it.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        set(MYNAH_CPU "x86-64-v3" CACHE STRING "CPU level for the CPU backend: x86-64-v3 (the package) or native (this machine)")
    else()
        set(MYNAH_CPU "native" CACHE STRING "CPU level for the CPU backend: native (x86-64-v3 is x86 only)")
    endif()
    set_property(CACHE MYNAH_CPU PROPERTY STRINGS x86-64-v3 native)
    if(MYNAH_CPU STREQUAL "native")
        set(GGML_NATIVE ON CACHE BOOL "" FORCE)
    elseif(MYNAH_CPU STREQUAL "x86-64-v3")
        set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
        foreach(feature GGML_SSE42 GGML_AVX GGML_AVX2 GGML_FMA GGML_F16C GGML_BMI2)
            set(${feature} ON CACHE BOOL "" FORCE)
        endforeach()
        # AVX-VNNI is not part of v3: Haswell through Zen 4 lack it. Whether
        # it is worth a second level is measured in Stage 5, not assumed.
        set(GGML_AVX_VNNI OFF CACHE BOOL "" FORCE)
    else()
        message(FATAL_ERROR "MYNAH_CPU is '${MYNAH_CPU}': use x86-64-v3 or native")
    endif()

    # The gpu tier (docs/ENGINE-MIGRATION.md, "Linux model tiers") runs on
    # ggml's Vulkan backend. On by default so a local build is the package
    # build; its shaders are compiled at build time with glslc.
    option(MYNAH_VULKAN "Build ggml's Vulkan backend (the gpu tier)" ON)
    if(MYNAH_VULKAN)
        # What ggml-vulkan's own CMakeLists requires, checked here first so
        # a missing one names its package instead of a config-file search.
        find_package(Vulkan COMPONENTS glslc QUIET)
        find_package(SPIRV-Headers CONFIG QUIET)
        if(NOT Vulkan_FOUND OR NOT SPIRV-Headers_FOUND)
            message(FATAL_ERROR
                "MYNAH_VULKAN needs the Vulkan headers, the SPIR-V headers and glslc.\n"
                "  Arch:  sudo pacman -S vulkan-headers spirv-headers shaderc\n"
                "Or build CPU-only with -DMYNAH_VULKAN=OFF.")
        endif()
    endif()
    set(GGML_VULKAN ${MYNAH_VULKAN} CACHE BOOL "" FORCE)
endif()
add_subdirectory(third_party/whisper.cpp)

# The Linux front end: only where it can build — its socket/injector layer
# is POSIX and compiles on the dev Mac; PipeWire capture and the `mynah`
# binary need libpipewire and stay Linux-only (see linux/CMakeLists.txt).
# MYNAH_BUILD_LINUX=OFF skips it entirely (the macOS app's build-core.sh,
# which only needs the core).
if(UNIX AND MYNAH_BUILD_LINUX)
    add_subdirectory(linux)
endif()

add_subdirectory(core)