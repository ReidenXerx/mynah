# Migrating the engine to C++

Status, 2026-09-22: **phases 1, 2, 5 and 6 done; 3 written but never built or
run on Linux; 4 not started; 0 skipped.** Phase 6 retired the Python package in
this commit — the engine, both front ends' tests and the golden corpus stand on
their own. What that leaves: Linux has no working dictation until phase 3 is
verified on a Linux machine (the Omarchy plugin's pinned install still fetches
the Python engine from an earlier commit), and phase 0's measurements — the
recognition test set, the latency baselines, the tier budgets and the real
benchmark clip — were never made, so every quality and tier claim below is
still unproven.

Original status: **plan, not started.** One dictation engine, written once in C++ behind a
C API, replacing the Python package on Linux and the Swift engine inside
`Mynah.app`. Each desktop keeps a front end in its own idiom.

## Why

The engine exists twice today — `mynah/engine.py` and the Swift
`SessionController` / `UtteranceDetector` / `TranscriptFilter` — and keeping the
two in step is where the bugs have come from: the 0.03 vs 0.010 floor, the
calibration window that swallowed the first second in Swift, and the
divergences that `tuning/golden/generate.py` still has to work around (trailing
silence trimmed in Swift and kept in Python; the min-utterance gate applied to
different buffers). Features have drifted too — only Python merges queued
utterances, spaces consecutive utterances, supports push-to-talk and measures
spectrum bands; only Swift has Silero VAD and trailing-silence trimming.

On Linux the Python engine also runs a `whisper-cli` process per utterance,
writing each one to a WAV on tmpfs and loading the model again every time.

## Target

```
                        ┌───────────────── libmynah (C++20, C API) ─────────────────┐
  16 kHz mono float ──► │ calibration → energy gates → segmentation → Silero VAD →  │
  commands          ──► │ whisper.cpp → hallucination filter → queue/merge/spacing  │ ──► events:
  config            ──► │ config.toml · model resolution (+ Linux tiers)            │     state, level+bands,
                        └────────────────────────────────────────────────────────────┘     text, problem, model

  macOS    Mynah.app (Swift)   capture · hotkey · typing · permissions · login item · UI · model download
  Omarchy  mynah (headless) + omarchy-mynah plugin (unchanged mechanism: CLI verbs + JSON lines)
  KDE      mynah-kde (Qt6/QML) capture · KGlobalAccel · tray · layer-shell pill · settings
```

Out of scope: GNOME, Windows, X11, Intel Macs.

## Decisions

### Made

| # | Decision |
|---|---|
| M1 | The engine is C++ behind a **C API**. Swift imports it through a module map, as it imports whisper.cpp today. |
| M2 | **Python leaves the product**, CLI included. Linux keeps a small native `mynah` binary because the Omarchy plugin and compositor bindings drive the engine through it (see Phase 3). |
| M3 | Targets: **macOS on Apple Silicon** (Swift app), **Omarchy** (existing plugin + headless binary), **KDE Plasma** (Qt app). Arch packages for Linux. |
| M4 | **macOS always runs `ggml-large-v3-turbo.bin`, unquantized, on Metal.** No tiers, no fallback down a preference list. |
| M5 | **Model tiers exist on Linux only**, chosen by hardware (see "Linux model tiers"). |
| M6 | whisper.cpp stays **pinned (v1.9.2) and statically linked** on every platform. A bump is deliberate and re-runs the recognition test set. |

### Proposed — confirm in Phase 0

| # | Proposal | Why |
|---|---|---|
| P1 | **C++20**, CMake, **doctest** vendored for tests. | Apple clang (Xcode 26+) and Arch's GCC/Clang both support it; a single-header test framework keeps the build offline, like the rest of the project. |
| P2 | **Segmentation policy = Swift's**: trim trailing silence to `trailing_padding` (0.2 s) and apply `min_utterance` to the trimmed buffer. Regenerate `expected.json` and pin the min-utterance gate, which is unpinned today. | Less silence handed to Whisper means fewer hallucinations, and one policy removes both documented divergences. |
| P3 | **VAD = Silero per utterance** through whisper.cpp's `whisper_vad_*` API (as `SileroVAD.swift` does), fail-open. Drop webrtcvad. | Level-independent, already in the pinned whisper.cpp, no extra dependency — and webrtcvad is what broke the Python install. |
| P4 | **Python's session features become the behaviour everywhere**: merge queued utterances (≤ 20 s), leading-space rule between utterances, push-to-talk, 12 spectrum bands. | macOS gains them for free. |
| P5 | **The core owns `config.toml`**: parsing, defaults, validation, read-modify-write preserving unknown keys, and the one-time import of `dictate_*` keys from `~/.config/whiz/config.toml`. Drop the Python-only keys `stt_provider`, `indicator`, `menu_bar`; keep `injector` (Linux). | Defaults are duplicated in `config.py` and `MynahConfig.swift` today. |
| P6 | **Control protocol v2 is a strict superset of v1** (see "Protocol"). | The pinned Omarchy plugin must keep working unchanged. |
| P7 | **Linux capture through PipeWire directly** (`libpipewire`), requesting 16 kHz mono. | Omarchy and Plasma both run PipeWire; removes the PortAudio layer. |
| P8 | **Downloads live in front ends, not the core.** The core publishes each model's URL, size and **SHA-256**; the Linux CLI downloads with libcurl, the Mac app with `URLSession`. Verify the hash. | The core stays free of network code; hashing fixes the "a 404 body saved as a model" class of bug properly. |
| P9 | **One version number**, from the CMake project, exposed as `mynah_version()`; `Info.plist` and `PKGBUILD` take it from there. | `pyproject.toml` says 0.1.0 and the app says 0.14.0 today. |

## Repository layout

```
core/
  include/mynah/mynah.h        the C API — the only header front ends include
  src/config/                  flat TOML, defaults, whiz import
  src/audio/                   SPSC ring buffer, RMS, 12-band spectrum
  src/segment/                 calibration, gates, utterance detector
  src/vad/                     Silero via whisper.cpp
  src/stt/                     whisper context, decode params, idle unload
  src/filter/                  hallucination filter
  src/session/                 state machine, worker, merge, spacing, auto-stop
  src/models/                  resolution, Linux tiers, benchmark, model table
  tests/                       doctest suites, golden + recognition runners
  tools/mynah-replay/          dev tool: WAV in → events and transcripts out
linux/
  common/                      PipeWire capture, control socket, injectors, downloads
  cli/                         the headless `mynah` binary
  kde/                         mynah-kde (Qt6/QML)
  packaging/                   PKGBUILDs, systemd user unit, .desktop files
macos/                         the Swift app, now linking libmynah
third_party/whisper.cpp        the submodule, moved from macos/vendor/
tuning/                        tuning.toml + golden corpus (generate.py stays, dev-only, stdlib)
```

## C API (v0 sketch)

```c
typedef struct mynah_engine mynah_engine;

typedef enum { MYNAH_IDLE, MYNAH_LOADING, MYNAH_LISTENING, MYNAH_TRANSCRIBING } mynah_state;

typedef enum {
    MYNAH_EVENT_STATE,    /* state */
    MYNAH_EVENT_LEVEL,    /* level 0..1, bands[12] */
    MYNAH_EVENT_TEXT,     /* utf8 text, spacing already applied — the front end types it */
    MYNAH_EVENT_PROBLEM,  /* stable code ("no_model", "model_load_failed", "no_audio", …) + message */
    MYNAH_EVENT_MODEL,    /* loading / loaded / unloaded, model name, tier */
} mynah_event_kind;

typedef struct mynah_event mynah_event;          /* accessors below, no public layout */
typedef void (*mynah_event_fn)(const mynah_event *event, void *user);

const char   *mynah_version(void);
mynah_engine *mynah_create(const char *config_path, mynah_event_fn on_event, void *user, char **error);
void          mynah_destroy(mynah_engine *);     /* frees whisper + VAD contexts before returning */

int  mynah_toggle(mynah_engine *);
int  mynah_start(mynah_engine *);
int  mynah_stop(mynah_engine *);                 /* returns at once; draining is reported as events */
int  mynah_ptt_press(mynah_engine *);
int  mynah_ptt_release(mynah_engine *);

/* Capture thread only. Never blocks, never allocates: copies into a ring buffer. */
int  mynah_push_audio(mynah_engine *, const float *samples, size_t count);

mynah_state mynah_get_state(const mynah_engine *);
int  mynah_reload_config(mynah_engine *);
```

Threading contract: events are delivered on engine threads and callbacks must
not block — front ends marshal to their UI thread. `push_audio` is the only call
made from the real-time capture thread. `destroy` must run before process exit:
ggml aborts at exit if a Metal context is still alive (Swift open issue 9).

## Protocol v2

Everything in `mynah/control.py` stays as it is: socket at
`$XDG_RUNTIME_DIR/mynah/control.sock`, 0600 in a 0700 directory the server must
own, refusing to steal a live socket, commands `toggle` `start` `stop` `status`
`quit` `subscribe`, bare words accepted, acknowledge first and act afterwards,
level events coalesced to 30 a second, subscribers dropped after a 0.25 s send
timeout. Events `state`, `level` (+ `bands`), `text`, `error` keep their shape.

Added, all ignorable by a v1 client:

- `subscribe` and `status` replies carry `"protocol": 2` and `"version"`.
- `{"event": "problem", "code": "no_model", "message": "…"}` — so front ends
  stop matching English on stderr (the plugin's `noteStderr` regex).
- `{"event": "model", "status": "loaded", "name": "ggml-small.bin", "tier": "small"}`.

## Linux model tiers

| Tier | Model | Chosen when |
|---|---|---|
| `gpu` | `ggml-large-v3-turbo.bin` | a GPU backend initialises **and** the benchmark meets the budget |
| `small` | `ggml-small.bin` | CPU, benchmark meets the budget |
| `base` | `ggml-base.bin` | otherwise |

- **Benchmark**, not guesswork: `mynah models benchmark` runs automatically on
  first start when `model` is unset, transcribes a bundled real-speech clip with
  each candidate, and stores the result. `model` in the config always overrides.
- **Budget** (seconds per utterance) is set from Phase 0 measurements. Starting
  point: `gpu` if ≤ 2 s, `small` if ≤ 5 s — `LINUX-APP.md` already prefers a
  correct sentence over a fast one.
- **GPU policy**: build the Vulkan backend into the package, but use an
  integrated GPU automatically (`GGML_BACKEND_DEVICE_TYPE_IGPU`, present in the
  pinned ggml) and a discrete one only when `gpu = true`. Waking a discrete GPU
  per sentence stays the owner's choice, as `LINUX-APP.md` says.
- **Unquantized only** (NS-15). Testing q8_0 for `small` is allowed in Phase 0;
  changing the rule is a separate decision.
- Each tier runs the recognition test set, since smaller models hallucinate
  differently and the filter and thresholds were verified on turbo only.

## Phases

Linux goes first: the Omarchy plugin is a finished acceptance test for the
engine, and Linux is where the per-utterance subprocess costs the most. macOS
goes last because its native engine already works.

### Phase 0 — Groundwork ⚠ skipped

- Update Xcode to 27 so `swift test` runs; set up CI: GitHub Actions on an
  Apple Silicon runner and an Arch container.
- **Recognition test set**: real recordings, Russian and English, with reference
  transcripts, including slang and obscenity to check the anti-censorship prompt
  (never validated under whisper.cpp). Kept out of the public repo — it is
  someone's voice.
- **Baselines**, recorded in this document: CER/WER and latency per utterance for
  Python on Linux (`whisper-cli`, `small` and `base`), the Swift app (turbo,
  Metal), and turbo on Linux CPU and on an integrated GPU via Vulkan.
- Confirm or change P1–P9.

**Exit:** decisions confirmed, baselines written down, test set exists.

### Phase 1 — Core skeleton ✅ done

- Move the whisper.cpp submodule to `third_party/`; CMake project for core and
  whisper.cpp; CI builds on both platforms.
- The C API header, with stubs.
- Port constants and pin them against `tuning.toml` (from `test_tuning.py` and
  `TuningTests.swift`).
- Hallucination filter, both match modes (from `TranscriptFilterTests.swift` and
  the Python tests).
- Config module (P5), with `ConfigTests.swift` cases ported. Round-trip check:
  a file written by the core is read correctly by the current Python and Swift.

**Exit:** `libmynah` builds on macOS and Arch, tests green.

### Phase 2 — The engine ✅ done (exit criteria need Phase 0's baselines)

- Calibration (speech-aware, capped at `calibration_speech_floor`), gates,
  utterance detector, spectrum bands.
- Regenerate the golden corpus under P2 and pin the min-utterance gate.
- Silero VAD per utterance, fail-open.
- STT with today's decode parameters: greedy, `no_context`, `no_timestamps`,
  `suppress_blank`, `suppress_nst`, `no_speech_thold` 0.35, `logprob_thold`
  −0.5, `flash_attn`, language, initial prompt with `DEFAULT_RUSSIAN_PROMPT` as
  the default.
- Model load/unload, 45 s idle unload, contexts freed on destroy.
- Session: states, toggle/start/stop/PTT, cancelling a start while the model
  loads (the generation counter from `SessionController`), auto-stop after
  silence, worker thread, queue merge ≤ 20 s, spacing rule.
- `mynah-replay` dev tool: a WAV through the full engine, events and transcripts
  out.

**Exit:** golden corpus green; recognition test set no worse than the Phase 0
baseline on turbo (Metal) and `small` (CPU); session tests clean under TSan and
ASan; short fuzz runs of `push_audio` and the config parser.

### Phase 3 — Headless `mynah` for Linux (Omarchy) ⚠ written, unverified on Linux

- PipeWire capture; control socket server (protocol v2); port the socket tests
  from `tests/test_linux.py`.
- Injectors ported from `linux_inject.py` / `linux_clipboard.py`: `smart`,
  `wtype`, `clipboard`; terminal paste chord; the keymap-deaf list; clipboard
  restored after ~1 s; an image on the clipboard left alone; Hyprland
  `hl.dsp.send_shortcut`.
- Commands. The plugin needs every one marked ●:

  | Command | Notes |
  |---|---|
  | `mynah` ● | runs the engine |
  | `--version` ● | exit 0 — the plugin's install probe |
  | `toggle` `start` `stop` `quit` ● | exit 0 once acknowledged |
  | `status` ● | prints the state, exit 0 when an engine answers |
  | `watch` ● | JSON lines until the engine exits |
  | `setup` ● | now read-only checks (alias `doctor`); packages cover dependencies |
  | `config` ● / `set` | show and change settings |
  | `service install` ● / `uninstall` / `status` | wraps `systemctl --user` on the packaged unit |
  | `models list` / `download` / `benchmark` | tiers, verified downloads |

- Until the plugin reads `problem` events, the stderr messages it matches (`not
  installed`, `No whisper model`, …) keep their wording.
- Packaging: systemd user unit at `/usr/lib/systemd/user/mynah.service` (same
  semantics as `mynah/systemd.py`); `PKGBUILD` for `mynah` (AUR, `-git` first):
  depends `pipewire wtype wl-clipboard curl vulkan-icd-loader`, makedepends
  `cmake git`.
- **Acceptance** — omarchy-mynah *at its current pinned commit*, against
  `/usr/bin/mynah`:
  - SUPER+ALT+D binds, toggles, and survives a config reload
  - bar states, the pill with bands and last text
  - adopts a systemd-owned engine; hands over on "Start it at login"
  - stop and start from the menu; a missing model shows up in the menu
  - latency no worse than the Python baseline for each tier
- Then a plugin release: install from AUR, pin a package version instead of a
  commit, read `problem` events (regex kept as a fallback).

**Exit:** AUR package published; Omarchy users can `pipx uninstall mynah`.

### Phase 4 — `mynah-kde` ⏳ not started

- Qt6/QML app linking `libmynah` and `linux/common`.
- Global shortcut through **KGlobalAccel**, default Meta+Alt+D (same as
  Omarchy), rebindable in System Settings; toggle or PTT.
- Tray through **KStatusNotifierItem**: bird by state; Dictate now, Settings,
  Start at login, Quit.
- Pill through **LayerShellQt**: port `Hud.qml`, `Spectrum.qml` and
  `BirdMark.qml` from omarchy-mynah, with `qs.Commons` replaced by Kirigami
  theming. The files are copied rather than shared at runtime — the plugin
  runs inside Quickshell's own Qt.
- Settings window: language, model/tier, trigger, sensitivity, auto-stop, idle
  timeout, indicator, and model download with progress.
- Autostart through an XDG autostart entry.
- Serves the same socket, so `mynah toggle` and scripts work. If the headless
  service already owns the socket, offer to stop and disable it.
- **Typing on KDE** (corrected 2026-09-23; see docs/LINUX-KDE-PLAN.md, Stage 3):
  KWin has no virtual keyboard, so `wtype` cannot work there. mynah pastes
  in-process instead: `ext-data-control-v1` for the clipboard and primary
  selection, KWin's `fake-input` for Shift+Insert, which KWin grants through
  the package's `.desktop` file with no dialog. Shared with the headless
  binary (`linux/common/kwin_backend.*`).
- `PKGBUILD` split package `mynah-kde`: depends `mynah qt6-declarative
  layer-shell-qt kglobalaccel kstatusnotifieritem kirigami`.

**Exit:** a fresh Arch + Plasma VM goes from package install to dictation
without opening a terminal.

### Phase 5 — macOS on the core ✅ done (no release zip built yet)

- Build: CMake builds core and whisper.cpp statically (Metal library embedded,
  `GGML_NATIVE=OFF`, deployment target 13.0); a `CMynah` module map replaces
  `CWhisper`; update `Package.swift`, `build-app.sh`, `dev.sh`.
- **Delete** `UtteranceDetector`, `TranscriptFilter`, `WhisperEngine`,
  `SileroVAD`, `WhisperModel`, `FlatTOML`, `MynahConfig`, and the engine half of
  `SessionController`, which becomes an adapter publishing core events to
  SwiftUI.
- **Keep** `AudioCapture` (now calls `push_audio`), `HotkeyManager` (plus key
  release for PTT), `TextInjector`, `Permissions`, `LoginItem`, the UI, and
  `ModelDownloader` (turbo and Silero only).
- Model per M4: turbo or a `no_model` problem that Settings turns into a
  download.
- Fix the leftovers: messages pointing at `mynah models download` now point at
  Settings; the "W" in `package.sh`'s INSTALL.txt.
- **Acceptance**: recognition test set matches the Phase 0 Swift baseline;
  hotkey, permissions and login item checked by hand; quitting with a model
  loaded exits 0; `package.sh` produces a working zip.

**Exit:** a release zip built on the core.

### Phase 6 — Retire Python ✅ done

- Delete `mynah/`, `tests/`, `pyproject.toml`, the LaunchAgent code.
  `tuning/golden/generate.py` stays as a stdlib-only dev tool.
- Migration notes: `pipx uninstall mynah`; unload
  `com.reidenxerx.mynah.dictate` / `com.reidenxerx.whiz.dictate` LaunchAgents
  on macOS.
- Rewrite `README.md`, `LINUX-APP.md`, `SWIFT-APP.md` and the install section of
  `docs/index.html`; write `docs/ARCHITECTURE.md`, which the code already cites
  but does not exist.

**Exit:** no Python in the product; CI green on both platforms.

## Compatibility during the migration

- `~/.config/mynah/config.toml` keeps its path and keys; every writer does
  read-modify-write, so Python, Swift and the core can share it while they
  coexist.
- The socket path and protocol v1 are unchanged, so a second engine still
  refuses to start instead of fighting over the microphone.
- The Python package stays installable until Phase 6; nothing is removed before
  its replacement passes acceptance.

## Risks

| Risk | Mitigation |
|---|---|
| Recognition quality regresses in the port | Phase 0 baselines; per-tier test-set gate in Phases 2, 3 and 5 |
| Threading bugs on the capture and worker threads | SPSC ring buffer, TSan/ASan in CI, fuzzing |
| KDE paste route for keymap-deaf apps does not pan out | Clipboard + notification fallback; `wtype` covers other apps |
| iGPU Vulkan slower or less accurate than expected | Tiers decided by benchmark, not assumption; CPU tiers stay |
| whisper.cpp behaviour changes on a bump | Pinned submodule (M6); bumps re-run the test set |
| Omarchy plugin breaks | Acceptance runs against its pinned commit; protocol only adds fields |
| Toolchain skew (as on the dev Mac: Xcode 26.6 vs CLT 27 SDK) | Xcode 27 in Phase 0; `dev.sh` SDK probe kept; CI pins the Xcode version |
