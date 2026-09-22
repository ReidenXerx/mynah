// mynah::ggml — registering ggml's compute backends, once, for the STT and
// the VAD alike.
//
// ggml ships Metal, Vulkan, BLAS and CPU as separately-loadable modules that
// are not registered automatically. Any whisper_*_init_* call made before
// ggml_backend_load_all() finds an empty device registry and hits
// GGML_ASSERT(device), which calls abort() — it does not return null, so it
// cannot be caught. Idempotent (an inline function's static is one object
// across translation units); the first call compiles the Metal library, or
// on Linux initialises Vulkan and wakes a sleeping dGPU, and can take seconds.

#pragma once

#include <cstdlib>

#include <ggml-backend.h>

namespace mynah::ggml {

inline void register_backends_once() {
    static const bool registered = [] {
#if !defined(__APPLE__)
        // A discrete GPU with Resizable BAR gets its buffers in VRAM the CPU
        // maps and memcpy()s into. Under NVIDIA's runtime D3 (the dGPU in
        // D3cold ten seconds after its last work) that path breaks: a model
        // reloaded after the GPU slept spun a core at 100% forever in that
        // memcpy (whisper.cpp alone reproduces it, RTX 5070, driver 610),
        // and a sentence after a sleep took 3.4 s instead of 1.2. Off, the
        // uploads go through a staging buffer and a GPU copy — no slower
        // awake (0.12 s vs 0.16 s for 10 s of speech on turbo). Read by ggml
        // when it creates a device, so it must be set before any exists.
        // Integrated GPUs (UMA) never take this path. A value the user set
        // is kept.
        setenv("GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM", "1", /*overwrite=*/0);
#endif
        ggml_backend_load_all();
        return true;
    }();
    (void)registered;
}

} // namespace mynah::ggml
