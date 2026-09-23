# Linux on KDE Plasma — closing the gaps

Status, 2026-09-23: **Stages 1 and 2 done** (a real voice went mic →
`small` on the CPU → typing queue → `text` event, from the build tree). The Vulkan build is still
unverified until `spirv-headers` is installed. Stages 3–8 not started;
decisions K3–K6 made. Scope is the Linux half of
[ENGINE-MIGRATION.md](ENGINE-MIGRATION.md) (Phases 3 and 4, and the Linux part
of Phase 0), targeted at a real machine rather than in the abstract. GNOME stays
out of scope. Omarchy/Hyprland keeps working through the code that is there,
but its acceptance run needs a Hyprland session and is not part of this plan.

## The reference machine

| | |
|---|---|
| OS / desktop | Arch, KDE Plasma 6.7.5, KWin on Wayland |
| CPU | Intel Core Ultra 9 275HX: 24 cores, AVX2 + AVX-VNNI, no AVX-512 |
| GPU | Intel Arrow Lake iGPU (Mesa ANV, Vulkan OK) + RTX 5070 Max-Q (nvidia-open 610; `vulkaninfo` lists only the iGPU) |
| Audio | PipeWire 1.6.8 + WirePlumber |
| Toolchain | GCC 16.2, Clang 22, CMake 4.4, Ninja |
| Missing | `wtype`, `wl-clipboard`, `spirv-headers`, `extra-cmake-modules` |
| KWin globals | `ext_data_control_manager_v1`, `zwp_input_method_v1`, `zwp_text_input_v2/v3`. **No `zwp_virtual_keyboard_manager_v1`** |
| Portals | RemoteDesktop, GlobalShortcuts, Clipboard, InputCapture (xdg-desktop-portal-kde) |

## What we found (2026-09-22, measured on this machine)

**Build.** Nobody has ever configured the Linux tree; the macOS build switches it off.
1. Configure fails. The `mynah` executable (`linux/CMakeLists.txt:37`) has the
   same name as the `mynah` library (`core/CMakeLists.txt:8`).
2. `MYNAH_VERSION` is only defined for the library and the core tests, so
   `linux/cli/main.cpp:50` fails to compile.
3. Three includes are missing, which Apple's libc++ hides because it includes
   more transitively: `<cmath>` in `core/src/segment/utterance_detector.cpp`,
   `<memory>` in `linux/common/injector.hpp`, `<cstdint>` in
   `linux/common/pipewire_capture.cpp`.
4. With 1–3 patched in a scratch worktree, everything builds. `mynah_tests` and
   `mynah-linux-tests` pass (GCC 16), and `--version`, `setup`, `status`,
   `models list` and `config` run.

**Typing: the headline gap.** `wtype` needs `zwp_virtual_keyboard_v1`, and
KWin 6.7 does not offer it. So on this desktop:
- every injector fails, including the wtype fallback for the paste chord;
- `setup` still reports typing as OK, because it only checks that the binary exists;
- ENGINE-MIGRATION.md (Phase 4, "KWin implements the virtual keyboard") and
  LINUX-APP.md (KDE ✅) are both wrong about this.

On top of that, the smart injector finds the focused app only through
`hyprctl`, so Konsole would get Ctrl+V, which does not paste there.

**Correctness bugs, confirmed.**
- `pipewire_capture.cpp:100`: `pw_stream_events` is a stack local handed to
  `pw_stream_add_listener`, which keeps the pointer. Use-after-free on the RT
  thread. The failure paths of `start()` also leak the loop and context. There
  is no `param_changed` handler, and `chunk->offset` is ignored.
- The `MYNAH_EVENT_TEXT` handler (`main.cpp:482`) types synchronously on the
  engine thread, for up to about 10 s. That breaks the "callbacks must not
  block" contract in `mynah.h`.
- `context.server` is assigned only after `server.start()`. A command that
  arrives in that window dereferences null.
- `auto_benchmark` (`main.cpp:175`) can never run. It exits early when a model
  resolves, but it only benchmarks when a model file exists, and then one
  always resolves.
- `resolve()` with no `model` set walks the preference list turbo-first and
  ignores tiers, so turbo would run on the CPU.
- `whisper_stt.cpp:37` always sets `use_gpu = true` on device 0, so the `gpu`
  setting is not honoured. With Vulkan, device 0 may be the RTX 5070.
- `mynah config` prints `frame_energy`, `min_energy` and `min_utterance` twice,
  and their descriptions shift by one row. `set` with no argument prints
  "unknown command". `watch --timeout abc` throws an uncaught exception.
- `downloads.cpp:34` matches `Content-Length` case-sensitively, so over HTTP/2
  (HuggingFace) the size check and the progress display are silently off.
  Every `sha256` in the model table is empty, so P8's hash check verifies nothing.

**Performance and packaging.**
- The PKGBUILD builds a static binary with `GGML_NATIVE=OFF` and no CPU level
  set. That is baseline x86-64: no AVX2 or VNNI for the CPU tiers.
- `GGML_VULKAN` exists only in the PKGBUILD, so a local build is CPU-only and
  differs from the package.
- `benchmark.wav` is a 2 s, 200 Hz placeholder tone, so tier selection has
  nothing real to measure.
- Both PKGBUILDs fail on the target clash. `depends` lists `wtype`, which is
  useless on KDE.

## Stages

Each stage ends in something that can be checked on this machine. The order is
chosen so dictation works end to end, through the headless binary and a KDE
shortcut, before any Qt code is written.

### Stage 1: It builds ✅ done (Vulkan build pending `spirv-headers`)

What landed, beyond the list below: `json::Value` defines its special
members after the class body, because Clang with libstdc++ 16 rejects the
implicit ones. The sanitizers also caught three real problems, all fixed:
the clipboard injector's leaked wtype fallback, and its detached restore
thread, which could outlive the injector (now one thread, joined on
destruction). While fixing that: two utterances within a second restored the
first dictated text instead of the user's clipboard, and now the first saved
content wins. Two socket tests and a clipboard test raced the code they
tested; they now wait for the condition instead.

Entry points: `make core-test` (GCC; Linux builds everything, including
`build/linux/mynah`), `make clang`, `make asan`, `make tsan`, each in its own
build tree. `CMAKE_FLAGS` passes extra options, e.g.
`make core-test CMAKE_FLAGS=-DMYNAH_VULKAN=OFF`.

- Rename the executable target to `mynah-cli` with `OUTPUT_NAME mynah`. Give
  it `MYNAH_VERSION`. Add the three missing includes.
- A CMake option `MYNAH_VULKAN`, default ON on Linux. This way the local build
  is the package build, and the PKGBUILD stops passing ggml flags on its own.
- CPU level for the static build: `MYNAH_CPU=x86-64-v3` (the default and
  the package: `GGML_NATIVE=OFF` plus AVX2, FMA, F16C, BMI2; AVX-VNNI is
  measured in Stage 5), or `MYNAH_CPU=native` for a build that only runs here.
- Makefile entry points for the Clang and sanitizer builds, and drop the stale `MYNAH_WITH_WHISPER`.
  Build under both GCC and Clang, and add ASan and TSan presets.
- `linux/tests/main.cpp` gets the same `MYNAH_CONFIG_DIR` sandbox as the core
  tests, so a test run cannot touch `~/.config/mynah`.

**Exit:** `make core-test`, `make clang`, `make asan` and `make tsan` are
green, with no sanitizer reports.

### Stage 2: Fix the confirmed bugs ✅ done

What landed:
- **Capture:** the rewrite also offers the format as `EnumFormat`, which is
  what `pw_stream_connect` expects, and logs stream errors. A live test
  starts and stops a real stream under ASan and TSan. PipeWire's
  unloaded modules leave LSan false positives, suppressed narrowly in
  `linux/tests/lsan.supp`; none of their stacks has a mynah frame.
- **Typing:** a `TypingQueue` types off the engine's thread, and on quit it
  finishes what is queued first.
- **Model and GPU:** `stt::pick_gpu` is the one policy, used by both the STT
  and the benchmark. An empty `model` on Linux means `kUntieredPreference`
  (small first).
- **Benchmark:** the old one never unloaded between candidates, and
  `load()` returns early when a model is loaded. So every candidate after
  the first re-timed the first model. It also saved a tier whose model
  might not be on disk, and it no longer does either.
- **Config numbers:** the formatter wrote `10.0` as `1e+01`, into
  `config.toml` too, and now writes numbers the way Python's `repr` does.
- **Downloads:** URLs are pinned to a revision, with the publisher's SHA-256
  and exact size (the same whisper.cpp revision `main` pinned in b7d498d).
  Content-Length is read in any case and reset on each redirect.
- **Checked live:** `small` and the Silero VAD downloaded and verified
  against the CDN. The stream negotiated; levels flow; the session
  goes loading → listening → idle; Silero rejects room noise.

Spoken check, 2026-09-23: two sessions from the owner reached the typing
queue and were published as `text` events. Several Silero segments per
session merged into one text, and slang and obscenity came through
verbatim (the Russian prompt at work). What it showed, for Stage 6:
- **A 0.1 s noise blip became "Счастье."** It happened in a 3 s session
  with nobody speaking: the energy gate let 0.35 s through, and Silero found
  one 0.10 s segment. `min_speech_duration_ms = 60` (`vad/silero.cpp`) is
  deliberately permissive so short words survive. Tune it against the
  recognition test set, not by guess: "да" and "нет" are about 0.2 s.
- **English spoken with `language = ru` comes out transliterated into
  Cyrillic.** That is expected with a pinned language. Mixed dictation needs
  `language = auto`, which the test set should measure on short utterances.

Found on the way, left for later:
- The macOS `ModelDownloader.swift` still downloads from `resolve/main`
  and checks no hash (P8).
- whisper.cpp logs every model load and VAD call to stderr, which is the
  systemd journal once the service runs (Stage 4).


Each fix comes with a test where one can be written.
- **PipeWire capture:** a static/member `pw_stream_events`, cleanup on every
  failure path, a `param_changed` handler that checks the negotiated format,
  and honouring `chunk->offset` and `size`.
- **Injection:** text events go to an injection queue thread, and the engine
  callback only enqueues. The server is wired up before it starts accepting.
- **Model selection:** `resolve()` becomes tier-aware. The first-run benchmark
  condition is fixed: benchmark when `model` is unset and no tier is stored.
  The engine honours `gpu` and picks the device explicitly: the iGPU when off,
  the dGPU when on.
- **CLI:** the `config` listing, `set` with no argument, `--timeout` parsing.
- **Downloads:** case-insensitive headers, and real SHA-256 values for the four
  models in `table.hpp`.

**Exit:** the tests pass under ASan and TSan. `mynah` runs from the build tree,
captures from the default mic and transcribes into the log, with typing
switched off.

### Stage 3: Typing on KWin (the real work; starts with a spike)

**Status, 2026-09-23: implemented; the hands-on paste test is still TO DO.**

What landed:
- **`PasteTyper`** (`linux/common/paste_typer.*`), the policy, is
  unit-tested over a fake backend. It saves both selections once per
  burst, sets both, presses the chord, and restores after 0.8 s; each
  paste pushes the restore back. A selection that cannot be saved is left
  alone, and an empty one is restored to empty.
- **`KWinBackend`** (`linux/common/kwin_backend.*`) runs its own Wayland
  connection and event thread, with `ext-data-control-v1` and
  `fake-input`. It saves every format of the user's clipboard (up to
  64 MB), so an image copied before dictating comes back, which
  `wl-copy` could not do. It answers from memory when it owns the
  selection itself.
- **`make_auto`** picks it on KWin, detected by KWin-only globals, since
  Hyprland serves some `org_kde_*` ones too. `make_smart` (wtype) stays
  for the rest, and for the tests, so a test run can never paste into the
  developer's focused window.
- **`mynah setup`** checks KWin's grant and prints the `.desktop` line for
  the running binary. The package installs `linux/packaging/mynah.desktop`
  for `/usr/bin/mynah`; `wtype` and `wl-clipboard` became optdepends.
- **Checked live, no keys pressed:** save → set a Cyrillic and emoji
  marker → Klipper reads it → restore → Klipper reads the user's text,
  which Klipper keeps after mynah exits.
- **Not yet checked live:** the chord itself, which needs a focused target
  (the test below). A development binary needs its own grant: `mynah
  setup` prints the line. This machine has
  `~/.local/share/applications/mynah-dev.desktop` for `build-cpu/`.

The test below can now also be done with mynah itself: run it, toggle, and
speak into each app.

**The spike** (below) came first:

What is already settled, from probing KWin 6.7.5 on this machine:
- `zwp_virtual_keyboard_v1` does not exist in KWin, even when requested, so
  `wtype` is out on KDE for good.
- KWin **grants** `org_kde_kwin_fake_input` (v6: `keyboard_key` and
  `keyboard_keysym`) and `org_kde_plasma_window_management` (v20, the
  focused window) to an executable whose installed `.desktop` file lists
  them in `X-KDE-Wayland-Interfaces`. There is no consent dialog, so this
  beats the RemoteDesktop portal (option b over a).
- `ext_data_control_manager_v1` is exposed to everyone: the clipboard and
  the primary selection can be read and set without focus.

The spike tool is `/tmp/kwtype/kwtype`. Its source is `/tmp/kwtype/kwtype.c`,
and its protocol XML was fetched from plasma-wayland-protocols and
wayland-protocols. It is registered by
`~/.local/share/applications/mynah-spike-kwtype.desktop`. (Also left from
the grant probe: `mynah-spike-waylandinfo.desktop`. Delete both, then run
`kbuildsycoca6`, when the spike is over.) `/tmp` does not survive a reboot:
rebuild with `wayland-scanner` + `gcc` as in the source header if needed.

**The test to run** (copy some text first, to check it is restored):

```
/tmp/kwtype/kwtype paste 5 "Привет, mynah! Hello."   # clipboard + primary, Shift+Insert
/tmp/kwtype/kwtype keysym 5 "Привет hello"            # keyboard_keysym per character
```

Run each, then focus the target during the 5 s countdown: Kate, Konsole
(at a prompt), Brave (a text field), Telegram (the message box; nothing is
sent), Warp. Run `keysym` once more with the `ru` layout active. Record
what appeared, and whether Ctrl+V still gives the copied text afterwards.
The results decide:
- whether Shift+Insert is the one chord for every app (K2), or terminals
  need detection through `org_kde_plasma_window_management`;
- whether `keyboard_keysym` types Cyrillic under `us`, and Latin under
  `ru`. If it does, text can be typed with no clipboard at all.

The default language is `ru`, so any route that turns text into keysyms breaks
on Cyrillic whenever the active layout is `us`. **The design is: put the text
on the clipboard, send a paste chord, restore the clipboard.** That route does
not depend on the layout, and it already exists as the `clipboard` injector.
The open question is only how to deliver the chord on KWin.

Spike, 1–2 days. Measure the UX of each option and pick one:
- **(a) RemoteDesktop portal.** `CreateSession` → `SelectDevices(keyboard,
  persist_mode=2)` → `Start`, keep the restore token in `$XDG_STATE_HOME/mynah`,
  and send the chord with `NotifyKeyboardKeycode`. It is standard and survives
  KWin changes, but costs one consent dialog. Check whether Plasma 6.7 honours
  the restore token across logout and reboot. D-Bus goes through sd-bus
  (libsystemd, already on every Arch system), so the headless binary stays Qt-free.
- **(b) KWin `org_kde_kwin_fake_input`.** This is a restricted global that KWin
  grants to clients whose `.desktop` file lists it in
  `X-KDE-Wayland-Interfaces`, which is how KDE Connect uses it. No dialog, but
  the interface is KDE-internal, and authorisation depends on how KWin matches
  the executable path to the `.desktop` file.
- **Which chord.** Either detect the focused app and send Ctrl+Shift+V in
  terminals, which needs a KWin script over D-Bus to read
  `workspace.activeWindow.resourceClass`. Or skip detection: put the text in
  both CLIPBOARD and PRIMARY and send **Shift+Insert**, which pastes in Qt,
  GTK, Chromium, Firefox, Electron and Konsole. Test both on the apps you
  actually use.

Then:
- Implement the winner as an injector in `linux/common`. `smart` picks it on
  KWin by checking the Wayland globals, not `$XDG_CURRENT_DESKTOP`; Hyprland
  keeps wtype. Use `wl-copy`/`wl-paste` for the clipboard (KWin has
  `ext_data_control_v1`).
- `setup`/`doctor` actually probes the typing route, the Wayland global or the
  portal, instead of checking that a binary exists.
- Correct LINUX-APP.md and ENGINE-MIGRATION.md Phase 4.

**Exit:** dictating Russian and English into Kate, Konsole, Firefox, Chromium
or Electron, and a Qt dialog, with a `us` layout active and with a `ru` layout
active. The clipboard is restored afterwards, and an image on it is left alone.

### Stage 4: Hotkey and login service on Plasma (small)

- Headless: bind `mynah toggle` to Meta+Alt+D. Either `mynah setup` prints the
  steps for System Settings → Shortcuts → Add Command, or the package ships a
  `.desktop` action with `X-KDE-Shortcuts`, which KGlobalAccel picks up.
  Push-to-talk needs key release, so it waits for Stage 8, or uses the
  GlobalShortcuts portal, which reports Activated and Deactivated.
- `mynah service install` on Plasma 6: check that `graphical-session.target` is
  reached, that the unit starts after login, restarts on a crash and stops on
  logout, and that PipeWire is available when it starts.

**Exit:** after a reboot and login, Meta+Alt+D dictates with no terminal open.

### Stage 5: Models, GPU and tiers on this hardware (medium)

- **Benchmark clip:** replace the tone with a real ~5 s clip of your own voice,
  which avoids licensing problems.
- **Vulkan on both GPUs:** why does `vulkaninfo` show only the iGPU? Check
  `nvidia_icd.json`, runtime power management, and whether the dGPU needs
  `__NV_PRIME_RENDER_OFFLOAD`. Then time turbo on the iGPU, turbo on the RTX
  5070, and small and base on the CPU with v3+VNNI.
- Set the tier budgets from those numbers; the "≤ 2 s / ≤ 5 s" figures are
  guesses today. Also decide the laptop policy: stay on the iGPU unless
  `gpu = on`, as planned, unless the numbers argue otherwise.

**Exit:** `mynah models benchmark` picks a tier on its own on first start, and
its choice matches the measurements.

### Stage 6: Phase 0, Linux half (medium; needs your recordings)

- **Recognition test set:** Russian and English, including slang and
  obscenity, with reference transcripts. Store it outside the public repo.
- A replay runner that feeds it through `mynah-replay` and reports CER and WER
  and per-utterance latency.
- **Baselines:** the retired Python engine, from a worktree at `1898b4f^` with
  `whisper-cli` small and base, against the C++ engine on each tier. Write the
  numbers into ENGINE-MIGRATION.md; that settles Phase 2's exit criteria for Linux.

**Exit:** the C++ engine is no worse than the Python baseline for each tier,
or the regression is understood and written down.

### Stage 7: Package it (small)

- PKGBUILD / PKGBUILD-git:
  - move `wtype` to optdepends (Hyprland);
  - add `wl-clipboard` and, if Stage 3 picked the portal, `xdg-desktop-portal`;
  - add `vulkan-headers`, `spirv-headers` and `shaderc` as makedepends (done in Stage 2);
  - add a `check()` that runs the tests;
  - install the `.desktop` file and the shortcut action.
- `makepkg -si` from the working tree, `namcap`, reinstall and upgrade over it.
- Publishing to the AUR is a separate decision. It is public and needs a
  release tag, and whisper.cpp here has no tags to fetch.

**Exit:** `pacman -S` of the local package gives working dictation after
login, and `pacman -R` leaves nothing running.

### Stage 8: `mynah-kde` (Phase 4) — built, running; turbo voice test next

**Status, 2026-09-23.** The app is in `linux/kde/`: Qt 6, QML, KF6 and
LayerShellQt. It is built by default on Linux (`-DMYNAH_KDE=OFF` for a
headless-only build) and packaged as the split package `mynah-kde`, which
depends on `mynah`. The spec came from the macOS sources (menu, pill,
settings, the state tints and every number) and is followed item for item:

- **Engine:** `linux/common/runtime.*` is the running engine (engine,
  typing, socket, capture, the ordering rules), shared with the headless
  CLI. So `mynah toggle` and `mynah watch` work against the app too.
- **Tray:** `KStatusNotifierItem`, one bird in the panel's text colour.
  The menu, in the macOS order: Start/Stop Dictation (with the shortcut),
  the state, the last error, "Typing: ready" (KWin's grant, the Linux
  twin of Accessibility), Start at Login, Settings…, Open Config File, the
  version and shortcut, and Quit mynah. When the headless service owns
  the socket, the menu offers "Stop It and Use This One". Checked over
  DBusMenu.
- **Shortcut:** KGlobalAccel `toggle-dictation`, default Meta+Alt+D,
  which can be changed in Settings or in System Settings. With `trigger =
  ptt`, press starts and release stops.
- **Pill:** a layer-shell overlay built to the macOS numbers (168×44
  capsule, 80 px up, the bird and 5 bars, the tints, 0.08 s and 0.18 s),
  click-through, with blur. Rendered offscreen and checked against the spec.
- **Settings:** General, Recognition and Sensitivity, with the macOS
  wording, ranges and "applies" notes. Linux additions: the KDE key
  recorder, the push-to-talk choice, the Linux models (turbo, small, base
  and Silero) with verified downloads, "Runs on", and the dGPU preference.
- **Lifecycle:** one instance per session (`KDBusService`; a second launch
  opens Settings), Start at Login as an XDG autostart entry, and a clean
  quit on SIGTERM and SIGINT.

Differences from macOS, on purpose: a left click on the tray icon starts
and stops dictation (Plasma trays are clicked, and there is no menu-bar
convention to keep); there is no "Reveal Log…" (the log goes to the
journal); and "Accessibility/Microphone" became "Typing".

Open:
- **The NVIDIA opt-in when launched from the menu.** This machine's
  `VK_LOADER_DRIVERS_DISABLE=*nvidia*` hides the dGPU from every app, and
  a launcher that wraps `env -u` may break KWin's Exec-path match for
  the fake-input grant. Until that is settled, run it with
  `env -u VK_LOADER_DRIVERS_DISABLE build/linux/kde/mynah-kde`.
- The turbo voice test, and the Stage 3 paste test, through the app.

The original plan for this stage:


Starts once Stages 1–5 hold. **The look follows the macOS app**
(`macos/`): its menu bar menu, settings window and indicator are the
reference, translated into Kirigami/QML. It is not a restyle of the Omarchy
plugin's pill. Follow ENGINE-MIGRATION.md Phase 4, with these changes:
- Typing uses the Stage 3 injector, not wtype.
- The shortcut goes through KGlobalAccel with press and release, so
  push-to-talk works. The tray uses KStatusNotifierItem, and the pill uses
  LayerShellQt (ported QML).
- The settings window takes over model download with progress, and autostart
  replaces the systemd unit for this front end.
- Needs `extra-cmake-modules` as a makedepend. Everything else is already
  installed here.

**Exit:** as in ENGINE-MIGRATION.md: a fresh Plasma VM goes from package
install to dictation without opening a terminal.

## The dGPU's sleep (measured 2026-09-23)

The goal: the dGPU is awake only while dictation needs it. NVIDIA's
fine-grained runtime D3 (`DynamicPowerManagement=3`, on here) already does
this below the level of mynah. A probe that walks ggml-vulkan's lifecycle
on the RTX 5070 read `power_state`:

| Phase | dGPU |
|---|---|
| Vulkan instance and device opened | D0 |
| 1.6 GB allocated (turbo loaded), then idle | **D3cold after ~8 s**, memory kept in self-refresh |
| Memory freed, device kept (ggml's state after the 45 s unload) | woke for the free, D3cold after ~12 s |
| Everything released | D3cold after ~10 s |

So there is no need to tie the dGPU to Whisper's lifetime: it sleeps about
ten seconds after the last sentence, with turbo still loaded. The 45 s idle
unload then also frees its video memory. ggml-vulkan caches its `VkDevice`
for the life of the process (`vk_instance.devices`), and the probe shows
that this does not keep the GPU awake.

With the Vulkan build and turbo on the RTX 5070 (2026-09-23):

- **ggml's Resizable-BAR path hangs after D3cold.** On a discrete GPU with
  ReBAR, ggml memcpy()s into VRAM the CPU maps. A turbo reload after the
  GPU had been in D3cold spun a core at 100% in that memcpy and never
  finished. whisper.cpp alone reproduces it, so it is not mynah's code.
  `mynah::ggml::register_backends_once` (`core/src/stt/backends.hpp`) now
  sets `GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM=1`, which sends uploads through
  a staging buffer:

  | 10 s of speech, turbo | ReBAR (default) | staging |
  |---|---|---|
  | transcription, GPU awake | 0.16 s | 0.12 s |
  | transcription, GPU in D3cold, turbo loaded | 3.42 s | 1.16 s |
  | reload after unload + D3cold | hangs | 3.27 s |

- **Vulkan starts with the engine**, not on the first toggle. It takes
  2.8 s with the dGPU in D3cold, and the first toggle paid it while audio
  was being dropped.
- **Toggle to listening, turbo not loaded, dGPU in D3cold: 3.2–3.4 s.**
  Audio before LISTENING is dropped, so after the 45 s unload the first
  ~3 s of speech are lost. This is the open problem (see K7).
- **K7, done:** capture now starts at the press, not at LISTENING. What is
  said during a cold load waits in the ring (now 30 s), and nothing is lost.
  Each session records the ring's write position at the press. Its worker
  waits for earlier sessions' workers to exit and then skips to that
  position, so the ring keeps a single reader. The old drain at activation
  was a second reader racing a stale worker. A press with the model
  already loaded calls `SpeechToText::wake()`: one encoder pass over 0.1 s
  of silence, which is a no-op off a discrete GPU. Live on the RTX 5070,
  with turbo loaded and the dGPU in D3cold, toggle → listening took 9 ms
  and the dGPU was in D0 0.4 s after the press. This is core code, so
  macOS gets capture-from-press too, if its capture already runs before
  LISTENING.
- **The first transcription in a new binary took 13.5 s**: NVIDIA compiles
  ggml's pipelines and caches them per application. It is paid once per
  install, not per session.

On this machine `~/.config/environment.d/90-gpu-default-igpu.conf` hides
the NVIDIA Vulkan driver from every app (`VK_LOADER_DRIVERS_DISABLE=*nvidia*`),
and apps opt in one by one. mynah does not override that. `mynah setup`
reports it, and the opt-in is `env -u VK_LOADER_DRIVERS_DISABLE` for now,
and an `UnsetEnvironment=` line in the service unit in Stage 4.

## Decisions to make

| # | Question | Recommendation |
|---|---|---|
| K1 | Chord delivery on KWin: portal (a) or fake_input (b)? | Decide after the spike; lean (a), because it is standard and survives KWin changes |
| K2 | Paste chord: detect terminals, or Shift+Insert with CLIPBOARD+PRIMARY? | Try Shift+Insert first: no window detection at all |
| K3 | Headless first, `mynah-kde` second? | **Decided 2026-09-22: yes** |
| K4 | CPU level of the packaged binary | **Decided 2026-09-22: x86-64-v3** (AVX2, FMA, F16C, BMI2). That drops Intel Core before Haswell (2013), AMD before Excavator (2015), and the Pentium/Celeron/Atom lines that shipped without AVX until about 2021. Those machines are too slow for `small` anyway |
| K5 | Use the RTX 5070 at all? | **Revised 2026-09-23: by default** (`gpu = on` is the new default). It wakes for a session and sleeps on its own afterwards, see "The dGPU's sleep" |
| K6 | The look of `mynah-kde` | **Decided 2026-09-22: follow the macOS app** |
| K7 | The first ~3 s of speech after the 45 s unload are dropped while turbo reloads | **Decided 2026-09-23: capture from the press, and wake the GPU at the press**; the 45 s unload stays. See below |
