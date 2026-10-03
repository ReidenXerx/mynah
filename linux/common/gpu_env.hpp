// mynah::gpu_env — the discrete GPU, when the session hides it from apps.
//
// Some setups keep the NVIDIA GPU asleep by hiding its Vulkan driver from
// every app (VK_LOADER_DRIVERS_DISABLE=*nvidia* in environment.d) and let
// apps opt in one by one. mynah's opt-in is its own `gpu` setting (on by
// default: an integrated GPU is too slow for turbo): with it on, mynah
// lifts the hiding for ITS OWN process only — the patterns that match
// NVIDIA's driver manifest are dropped, every other pattern kept — before
// anything starts Vulkan. That covers every way mynah is started (the
// menu, Start at Login, the systemd unit, a terminal) with no launcher
// wrapping, which would cost KWin's typing permission (kwin_backend.hpp).
// The dGPU still sleeps between sentences (docs/LINUX-KDE-PLAN.md).
//
// Must run before the first Vulkan instance: the loader reads the
// variable once, when it starts, so a change later in the process does
// nothing until mynah restarts.

#pragma once

#include <string>

namespace mynah::gpu_env {

// Apply the `gpu` setting to this process's environment: when on and the
// NVIDIA driver is hidden, un-hide it. Call once, early.
void apply(bool discrete_gpu);
// apply() with the `gpu` of the user's config.toml — what the programs
// call, so a Qt front end need not include the core's config headers
// (Qt's `emit` macro breaks them).
void apply_configured();

// Whether the session hid NVIDIA's driver from this process at start
// (before apply()), and whether apply() lifted that.
bool was_hidden();
bool unhidden();

// The pure part: a VK_LOADER_DRIVERS_DISABLE value without the patterns
// that match `manifest` (comma-separated globs, as the Vulkan loader
// reads them; matched against the driver's manifest file name).
std::string without_matching(const std::string& patterns, const std::string& manifest);

// NVIDIA's Vulkan driver manifest, as the loader names it.
inline constexpr const char* kNvidiaManifest = "nvidia_icd.json";

} // namespace mynah::gpu_env
