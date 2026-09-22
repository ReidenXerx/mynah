// mynah::config — the core owns config.toml (P5).
//
// The file lives at ~/.config/mynah/config.toml and has more than one writer:
// this core, the Python CLI (until Phase 6), the Swift app (until Phase 5),
// and whatever version of any of them happens to be installed. Two rules
// follow from that, pinned by core/tests/test_config.cpp (ported from
// MynahConfigTests in macos/Tests/MynahAppTests/ConfigTests.swift) and the
// Python suite:
//
//   1. Save is read-modify-write. Saving re-reads the file and overwrites
//      only the keys below, leaving every other key (another writer's
//      setting) untouched. A plain overwrite would silently delete the
//      others' settings.
//   2. Load is type-tolerant. A missing file means defaults; a value of the
//      wrong type falls back to the default for that key rather than taking
//      the app down.
//
// Mynah grew out of whiz dictate, so the first run imports the dictate_*
// keys from ~/.config/whiz/config.toml — nobody should have to pick their
// hotkey a second time because a project was split in two.
//
// Keys dropped from the Python era (P5): stt_provider, indicator, menu_bar —
// front-end concerns the core no longer owns. `injector` stays (Linux front
// end). Defaults mirror mynah/config.py and MynahConfig.swift, pinned against
// tuning/tuning.toml.

#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include "flat_toml.hpp"

namespace mynah::config {

struct Config {
    // --- what listens ---
    // Global hotkey spec. '.' is a literal character, NOT a named key.
    std::string hotkey = "<cmd>+<shift>+.";
    // "toggle" (press to start, press again to stop) or "ptt" (hold to talk).
    std::string trigger = "toggle";
    // Transcription timing (Dudu's request): "live" transcribes each
    // utterance as you pause (the default, unchanged behaviour); "on_stop"
    // buffers the whole session and transcribes once, when the session ends
    // — one coherent decode with full context, at the cost of waiting for
    // the text until afterwards. The Python engine's vad=false mode was
    // exactly this.
    std::string transcription_mode = "live";
    // VAD for utterance segmentation (P3: Silero per utterance).
    bool vad = true;
    // Seconds of continuous silence before a session stops itself (0 = off).
    double auto_stop_silence = 10.0;

    // --- what it hears ---
    std::string language = "ru";
    // Speech model: a name the resolver knows, or a path. Empty = default.
    std::string model;
    // initial_prompt to bias recognition. Empty = the built-in one.
    std::string prompt;
    // Seconds to keep the model loaded after a session (0 = never unload).
    double idle_timeout = 45.0;

    // --- how loud is speech ---
    // Static floors (normalized RMS, 0.0-1.0): the adaptive calibration at
    // session start can raise the effective gates above them, never lower.
    double frame_energy = 0.010;
    double min_energy = 0.008;
    // Shortest utterance worth transcribing, in seconds.
    double min_utterance = 0.25;

    // --- what you see ---
    bool show_indicator = true;
    bool idle_visible = false;

    // --- which implementation (Linux) ---
    std::string injector;
    // Linux (M5): prefer a discrete GPU for speech when there is one; off
    // means an integrated GPU or the CPU only. On by default since
    // 2026-09-23: NVIDIA's fine-grained runtime D3 puts the dGPU into D3cold
    // about ten seconds after its last work, even with turbo still loaded
    // (measured on an RTX 5070 laptop), so the fan cost that made this
    // opt-in is a few seconds per session, not the session's length.
    // Ignored on macOS, which is always Metal (M4).
    bool gpu = true;

    bool operator==(const Config&) const = default;
};

// What went wrong reading the file (M10, from the Swift port): a MISSING file
// is normal first launch and means defaults; an UNREADABLE one (permissions,
// EISDIR, I/O) must be surfaced, not swallowed — a silent reset to defaults
// makes every saved setting quietly vanish.
enum class ReadStatus { ok, missing, unreadable };

struct ReadResult {
    Config config;
    ReadStatus status = ReadStatus::ok;
    // Human-readable, names the file; empty when status is ok or missing.
    std::string message;
};

// Thrown by load() and save() — the failures the Python side raises on and
// the C API turns into an error string.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ~/$MYNAH_CONFIG_DIR/config.toml, or ~/.config/mynah/config.toml.
// Honoured exactly as mynah/config.py honours it, so tests and alternate
// profiles behave identically on every side.
std::filesystem::path default_path();

// $MYNAH_LEGACY_CONFIG or ~/.config/whiz/config.toml — where the settings
// lived while dictation was part of whiz.
std::filesystem::path legacy_path();

// Read one file. No import, no write: the sharable primitive.
ReadResult read_file(const std::filesystem::path& path);

// Config values from a parsed table, falling back to defaults for anything
// absent or of the wrong type (Swift's MynahConfig.from).
Config from_values(const flat_toml::Table& values);

// Write this config's keys into a table parsed from the current file,
// preserving every key we do not own (the read-modify-write half of save()).
void merge_into(flat_toml::Table& values, const Config& config);

// Settings from disk or the defaults, with the one-time whiz import (see
// file comment). Throws Error if the file exists but cannot be read.
Config load();

// Save at default_path(), preserving unknown keys. Throws Error on I/O
// failure. The write is atomic (a uniquely named temp file + rename) so a
// crash mid-write cannot leave a torn config behind. A symlinked config is
// written through to its target, and the file's permissions are kept.
void save(const Config& config);

// Save at `where` instead of default_path(), with the same semantics. A
// front end created against an explicit config file writes back to THAT
// file: saving its settings into ~/.config/mynah/config.toml instead would
// change a file nobody asked about and lose the change on the next reload.
void save(const Config& config, const std::filesystem::path& where);

} // namespace mynah::config