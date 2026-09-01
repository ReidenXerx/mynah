# The macOS Swift app

`macos/` holds a native menu bar app that is replacing the PyObjC dictation
daemon. The Python package is unchanged and still installs and runs on its own —
this is additive until the cutover in phase 3.

## Why

The dictation daemon is a Python process impersonating a Mac app, and most of
its complexity is the cost of that impersonation:

- `dictate/service.py` copies Apple's framework Python binary to
  `~/.local/share/whiz/whiz` so Activity Monitor shows "whiz" instead of
  "Python", and keeps it at a fixed path so the TCC Accessibility grant survives
  `pipx install --force`.
- `providers/macos_indicator.py` drives AppKit through stringly-typed selectors
  (`performSelectorOnMainThread_withObject_waitUntilDone_("whizFadeIn:", …)`),
  with instant-show fallbacks because CoreAnimation fails to load under launchd.
- `providers/macos_rumps.py` exists because hand-driving `NSStatusItem` through
  PyObjC did not work; `providers/macos_menubar.py` was the abandoned attempt.
- Any exception inside an AppKit callback kills the process, which a `KeepAlive`
  LaunchAgent then restarts — so the code is defensively wrapped throughout.

An app bundle has a name, a stable identity, and a working animation backend by
construction, so all of the above stops being necessary rather than being fixed.

## What is in the scaffold

| Area | File | Replaces |
|---|---|---|
| Entry point, menu bar item | `WhizApp.swift` | `macos_rumps.py` |
| Menu contents | `UI/MenuBarContent.swift` | `macos_rumps.py` |
| Floating pill | `UI/IndicatorPanel.swift` | `macos_indicator.py` (519 → ~150 lines) |
| W monogram | `UI/WhizLogo.swift` | `macos_logo.py` |
| Session state | `Session/SessionController.swift` | part of `engine.py` |
| Mic level | `Session/MicLevelMonitor.swift` | part of `engine.py` |
| Global hotkey | `Input/HotkeyManager.swift` | `pynput` |
| Text injection | `Input/TextInjector.swift` | `macos_inject.py` |
| Accessibility | `System/Permissions.swift` | `macos_inject.py` |
| Start at login | `System/LoginItem.swift` | `service.py` (343 lines → 2 calls) |
| Config | `Config/FlatTOML.swift`, `Config/WhizConfig.swift` | shares `config.py`'s file |
| Speech recognition | `STT/WhisperEngine.swift`, `Sources/CWhisper` | `providers/mlx.py` |
| Voice activity detection | `STT/SileroVAD.swift` | `webrtcvad` via `vad.py` |
| Model resolution + download | `STT/WhisperModel.swift`, `STT/ModelDownloader.swift` | `whiz models download` |
| Language list | `STT/WhisperLanguages.swift` | — |
| ggml backend registration | `STT/GGMLBackends.swift` | — |
| Gates + hallucination filter | `STT/TranscriptFilter.swift` | `engine.py` constants |
| Utterance segmentation | `STT/UtteranceDetector.swift` | `vad.py` + `engine.py` |
| Mic capture (16 kHz mono) | `Session/AudioCapture.swift` | `sounddevice` |
| Settings window | `UI/SettingsWindow.swift`, `UI/SettingsView.swift`, `UI/ModelSectionView.swift` | — |

Speech recognition is wired and has been exercised by hand, including in a room
with a robot vacuum running. What remains unverified is called out in
"Open issues" below.

## Deployment target

**macOS 13.0 (Ventura).** That is the oldest release carrying both `MenuBarExtra`
and `SMAppService` — the two APIs that let us delete `macos_rumps.py` and
`service.py`. Targeting 12 or lower means hand-rolling `NSStatusItem` and
LaunchAgent plists again, which is precisely what this port removes. Ventura
also still covers Macs back to 2017.

The only cost of 13 over 14 is using `ObservableObject`/`@Published` instead of
the `@Observable` macro, and the single-parameter form of `onChange`. Both are
marked in the source. Neither is user-visible, so do not "modernise" them
without deciding to drop the older hardware.

Everything else in the app reaches back much further — `AVAudioEngine`,
`CGEvent`, Carbon's `RegisterEventHotKey`, `AXIsProcessTrustedWithOptions`,
`NSVisualEffectView` and SwiftUI `Shape` are all macOS 10.15 or earlier, and
Swift 6 concurrency back-deploys. Phases 3 and 4 do not raise the floor either:
whisper.cpp's Metal backend needs roughly macOS 11+, and python-build-standalone
targets 10.15/11.

## Decision 1 — speech recognition engine: whisper.cpp

**Status: decided, provisional.** This is the *first* engine decision, not a
settled one. It was made on the evidence below; if testing contradicts it,
change it. Record what testing finds in "Open issues" at the end of this
section — that log is the input to any revision, and an empty log means nobody
has actually tried it yet.

### Chosen

whisper.cpp, linked in-process through its C API (`Sources/CWhisper`), running
`ggml-large-v3-turbo.bin` at **full precision**.

### Why

The criterion was reuse: which engine preserves the tuning already paid for in
`whiz/dictate/`? That tuning is the asset, not any particular runtime.

| Asset | Origin | Transfers to whisper.cpp? |
|---|---|---|
| large-v3-turbo, unquantized | commit `ea49da8` | Yes — identical OpenAI weights, ggml build |
| Russian anti-censorship prompt | `DEFAULT_RUSSIAN_PROMPT` | Yes — `whisper_full_params.initial_prompt` |
| 25-phrase hallucination blocklist | `engine.py` | Yes — **verified**, see below |
| Energy gates + adaptive noise floor | `engine.py` | Yes — pre-STT, engine-agnostic |
| VAD segmentation | `vad.py` | Yes, and whisper.cpp 1.9.2 ships Silero VAD natively |

Supporting reasons:

- **Already a dependency.** whiz requires `whisper-cli` for batch transcription
  and every entry in `models.py:KNOWN_MODELS` is ggml. Dictation previously kept
  a *second* model in a *second* format (mlx safetensors, 1.6 GB, under
  `~/.cache/huggingface`). One engine now means one model file.
- **Not Apple-Silicon-only.** mlx is. Since Windows and Linux apps are planned,
  choosing mlx would guarantee writing a second backend later.
- **Size.** Drops torch, numba, scipy, llvmlite and sympy — about 1.1 GB of the
  1.2 GB pipx venv.

### What was rejected, and why

- **mlx-whisper.** Faster than whisper.cpp in Python on Apple Silicon, which is
  a real result but does not transfer: there is no mature Swift port, so keeping
  it means keeping Python in the hot path forever. Apple-Silicon-only.
- **WhisperKit.** Swift-native CoreML, a reasonable middle ground, but
  Apple-Silicon-only and needs model conversion. Revisit only if whisper.cpp's
  Metal performance disappoints.
- **Streaming transducers** (sherpa-onnx Zipformer, Nemotron ASR). Genuinely
  attractive — live text as you speak — but two structural blockers: transducers
  have no `initial_prompt`, so the anti-censorship strategy would have to be
  rebuilt as contextual biasing; and they emit lowercase, unpunctuated text,
  which needs a separate punctuation-restoration model before it is usable for
  dictation.

### Evidence

Commit `ea49da8` is the strongest data point, and it is about the *model*, not
the runtime: 4-bit turbo produced "garbled mixed-language output on real speech"
because turbo has only 4 decoder layers and quantizes badly. That is why
`WhisperModel.preference` puts unquantized turbo first, unlike
`models.py:PREFERENCE` which prefers `q5_0` for batch speed.

Measured here, `ggml-large-v3-turbo-q5_0.bin` on an M4 Pro, 4 s of pink noise at
amplitude 0.02 (roughly cooler-fan level):

```
load:       0.25 s   (warm; first-ever load compiles the Metal library, ~7 s)
transcribe: 0.64 s
raw output: "Субтитры создавал DimaTorzok"
energy gate would transcribe: false
hallucination filter catches it: true
```

The hallucination is the useful part. whisper.cpp reimplemented Whisper's
decoding loop independently of the reference implementation mlx-whisper ports,
so it was an open question whether a blocklist tuned against mlx would catch
whisper.cpp's artifacts. It emitted a phrase from the *same* subtitle-credit
family the list already covers, and both the energy gate and the filter rejected
it. The ported tuning transfers.

### Still unvalidated

Nobody has run **real Russian speech** through this. The above proves the
binding works and that noise is rejected; it says nothing about recognition
quality, and specifically nothing about whether `initial_prompt` suppresses
self-censoring of slang and obscenity under whisper.cpp as it does under mlx.
That test is the gate on this decision, and it has not been run.

### Open issues

Findings from testing go here. Add to this list rather than editing the decision
above, so the reasoning and the evidence against it stay separately readable.

1. ~~**Homebrew's dylibs are built for macOS 26**~~ — **fixed** by vendoring; the
   binary now reports `minos 13.0` and has no Homebrew dependency.
2. **ggml backends need explicit registration.** Metal, BLAS and CPU ship as
   loadable modules in `$(brew --prefix)/Cellar/ggml/*/libexec/*.so` and are not
   registered automatically. `WhisperEngine` calls `ggml_backend_load_all()`
   first; without it the device registry is empty.
3. **whisper aborts rather than returning null** when no backend device exists —
   `GGML_ASSERT(device)` calls `abort()` inside
   `whisper_init_from_file_with_params`, killing the process. It cannot be
   caught, so the backend registration in issue 2 is load-bearing, not
   defensive.
4. ~~**Homebrew version skew.**~~ — **fixed** by vendoring; the build is pinned to
   the submodule at v1.9.2.
5. **First-ever model load takes ~7 s** compiling the Metal library, versus
   ~0.25 s once cached. Cold start needs UI feedback or a warm-up at launch.
6. ~~**No way to obtain models from the app.**~~ — **fixed**; see "Models" above.
7. **Text injection fails silently without Accessibility.** CGEvent posting
   is a no-op when the process is untrusted — no error, no exception, nothing
   typed. Found in first real testing: STT ran correctly and logged
   "injecting 13 chars" while nothing reached the focused app. `TextInjector`
   now refuses to run and logs when untrusted, and `SessionController` warns at
   session start. Compounding it, the only route to granting the permission was
   a menu item inside a menu that would not open (see below).
8. **First real-testing bugs, fixed.** The menu never opened because
   `AppDelegate` was not `ObservableObject` and its `controller` was `nil` at
   first render, so SwiftUI cached an empty menu. The pill never appeared
   because `handleTrigger` read `isSessionActive` synchronously right after
   `toggleSession()`, which became async when model loading moved off the main
   thread — it always read the stale value and hid the pill immediately. The
   microphone prompt appeared during the first capture, which yields a session
   of silence; permission is now awaited before the engine starts.
9. **ggml aborts at process exit if a model is still loaded.**
   `ggml_metal_device_free` asserts when a device is torn down with residency
   sets still registered, and that runs from a static destructor — so quitting
   with a model loaded turned a clean quit into `SIGABRT` and a crash report.
   Measured: exit code 134 without unloading, 0 with.
   `SessionController.shutdownBlocking()` now frees both contexts on terminate.
   Found while testing session cancellation, not by review.
10. **`Package.swift` was unbuildable and nobody noticed**, because the local
   SwiftPM install was broken so no one could run it. Two defects: paths in
   `unsafeFlags` are *not* resolved relative to the package root (now derived
   from `#filePath`), and a `.systemLibrary` target accepts no `cSettings` — so
   `#include <whisper.h>` in the module map could not be satisfied by any flag
   SwiftPM allows. The module map now names the vendored headers by a path
   relative to itself, and the `shim.h` indirection is gone.
11. **The calibration window used to discard the first ~1 s of speech.** Fixed —
   it now samples the ambient level and falls through to segmentation in the
   same frame, matching `engine.py`. Anyone who pressed the hotkey and started
   talking immediately lost their opening word.
12. **Settings text fields wrote on every keystroke.** Each edit saved the config
   file, briefly persisting partial values (`r`, then `""`, while retyping a
   language code). Now committed on Return or focus loss, with the hotkey field
   validated through the same `HotkeySpec.parse` that registration uses.
13. **Opening Settings from a second Space switched the user to the Desktop**
   and left the window behind. The window now uses `.moveToActiveSpace` and is
   ordered in before the app is activated.
14. **`models.py:PREFERENCE` prefers `q5_0` for batch.** Same model class
   `ea49da8` found garbles Russian in dictation. q5_0 is milder than q4 and may
   be fine, but the batch default now contradicts the dictation finding and
   nobody has checked. Independent of this decision, worth testing.

## Two-stage speech detection

Segmentation and speech detection are deliberately separate, because they answer
different questions at different costs:

| Stage | Question | Cost | Where |
|---|---|---|---|
| Energy gates | *when* does an utterance start and end? | per audio buffer | `UtteranceDetector` |
| Silero VAD | *is* this a human voice? | once per utterance | `SileroVAD` |

The energy gates only measure loudness, so a fan, a door or a keyboard passes as
readily as speech — and near-silent noise handed to Whisper is precisely what
produces its subtitle-credit hallucinations. Silero is a 0.8 MB model that judges
by the shape of the sound rather than its level, and runs in 20–45 ms against a
~600 ms transcription.

Measured on synthetic input: pink noise at RMS 0.0037 and 0.0090 and near-silence
at 0.0002 are all rejected; a 200 Hz sine tone is a **false positive**, since it
sits in the male vocal pitch range. Real rooms rarely contain sustained pure
tones, but it is not infallible.

It **fails open**: if the model is missing or fails to load, utterances pass
through rather than being silently swallowed. The worst case is the old
behaviour, not a mute app.

Being level-independent also makes Silero the right pairing for Apple's voice
processing / AGC, should that ever be added — AGC deliberately destroys the
stable relationship between loudness and speech that the energy gates depend on.

The model is the same one the batch pipeline downloads via
`whiz models download-vad`; there is no second asset.

## Models

`ModelDownloader` fetches from the same HuggingFace repositories as
`whiz/models.py`, into the same `~/.cache/whisper`. One cache, both tools:
`whiz models list` sees what the app downloads and vice versa. This removed the
last reason a dictation-only user needed the Python package installed.

`WhisperModel.preference` puts **unquantized** turbo first, unlike
`models.py:PREFERENCE` which prefers `q5_0` for batch speed. That is deliberate —
see the evidence in Decision 1.

Non-2xx responses are rejected before the file is moved into place. A 404 body is
a perfectly valid small file, and without that check it lands on disk named
`ggml-large-v3-turbo.bin` and fails much later with a baffling error.

## Vendored whisper.cpp

`macos/vendor/whisper.cpp` is a submodule pinned to **v1.9.2**, built statically
by `scripts/build-whisper.sh`. Linking Homebrew's copy made the app unshippable:
absolute `/opt/homebrew` paths (broken on Intel, or on any machine without
`whisper-cpp`), binaries built for a much newer macOS than the 13.0 floor, and
ggml's compute backends living as loose `.so` files in `Cellar/*/libexec`.

Three cmake options do the work:

- **`BUILD_SHARED_LIBS=OFF`** — static ggml compiles the compute backends in and
  registers them directly. This single flag removed the entire bundling problem:
  no dylibs to copy into `Contents/Frameworks/`, no `install_name_tool`, no
  `@rpath`, no loadable modules to locate.
- **`GGML_METAL_EMBED_LIBRARY=ON`** — Metal shaders compiled into the binary.
  Otherwise ggml looks for `ggml-metal.metal` at runtime and silently drops to
  CPU.
- **`GGML_NATIVE=OFF`** — required for two independent reasons. A native build
  bakes in the build machine's CPU features and can fault on an older Mac, which
  defeats the point of vendoring. And ggml's native detection probes features by
  compiling *and running* test programs — its SVE probe **hangs forever** on
  Apple Silicon, spinning at 100% CPU instead of faulting, so cmake never
  returns. Configure went from hanging indefinitely to 1.5 seconds.

`-lc++` is also needed: whisper.cpp and ggml are C++, and Swift does not link
libc++ for a static archive reached through a C module map.

Result: a 3.4 MB bundle with **no Homebrew dependency** and `minos 13.0`.

Bumping the submodule is a deliberate act — re-run the Russian test set
afterwards, since whisper.cpp's decoding loop differs from the reference
implementation and hallucination behaviour can change between releases.

## Config is co-owned

Both binaries read and write `~/.config/whiz/config.toml`. Python owns the
pipeline keys, Swift owns `dictate_*`, and neither may clobber the other's, so
`WhizConfig.save()` is always read-modify-write.

`FlatTOML.swift` is a ~120-line parser rather than a TOML dependency, for the
same reason `config.py` hand-rolls `_emit_toml`: the schema is one flat table of
scalars and string arrays. It keeps the package dependency-free and buildable
offline. Round-trip compatibility in both directions is what
`Tests/WhizAppTests/ConfigTests.swift` exists to pin.

Defaults are duplicated in `WhizConfig` and must be kept in step with
`whiz/config.py`. If they drift, the same file means two different things
depending on which binary read it.

## Building

```sh
macos/scripts/build-app.sh          # debug
macos/scripts/build-app.sh release  # release
open macos/build/Whiz.app
```

The script prefers SwiftPM and falls back to invoking `swiftc` over the sources
directly when SwiftPM is unavailable (see below). Either path produces the same
binary; the fallback just cannot run the test suite. It then wraps the binary
into `Whiz.app` with `Info.plist` and ad-hoc signs it.

Run the bundle, not the raw binary — TCC keys permissions to the bundle
identifier.

### Testing a local build

1. **Stop the Python agent first.** Both register the same global hotkey and
   whichever starts first wins, so leaving it running makes the Swift app look
   broken:
   ```sh
   launchctl unload ~/Library/LaunchAgents/com.reidenxerx.whiz.dictate.plist
   ```
2. **Get the right model.** `WhisperModel.preference` wants unquantized turbo;
   without it the resolver silently falls back to `q5_0`, which commit `ea49da8`
   warns about for Russian (open issue 6):
   ```sh
   whiz models download ggml-large-v3-turbo.bin
   ```
3. `open macos/build/Whiz.app` — a W appears in the menu bar; there is no Dock
   icon or window (`LSUIElement`).
4. Grant **Accessibility** (menu → "Grant Accessibility…") and allow the
   microphone at the first prompt. Ad-hoc signatures change on every rebuild, so
   macOS treats a rebuilt app as a new one — expect to remove and re-add it in
   System Settings after rebuilding.
5. Focus a text field, press the hotkey (default `⌘⇧.`), speak, press again.

To restore the Python agent:
```sh
launchctl load ~/Library/LaunchAgents/com.reidenxerx.whiz.dictate.plist
```

### Known environment issue

SwiftPM does not work with Command Line Tools alone — even a three-line package
fails to link its manifest against `libPackageDescription`. `build-app.sh`
detects this and falls back to `swiftc`, so building the app works either way;
only `swift test` is blocked. Full Xcode fixes it:

```sh
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

Until then the sources can still be type-checked directly, which is how the
scaffold was verified:

```sh
swiftc -sdk "$(xcrun --show-sdk-path)" -target arm64-apple-macosx13.0 \
  -swift-version 6 -parse-as-library \
  -Xcc -I/opt/homebrew/include -I macos/Sources/CWhisper \
  -L/opt/homebrew/lib -lwhisper -lggml -lggml-base \
  $(find macos/Sources/WhizApp -name '*.swift') -o /tmp/WhizApp
```

### Running the tests

```sh
swift test --package-path macos     # 18 tests in 3 suites
```

This needs **full Xcode**, not just Command Line Tools — and for two separate
reasons, both discovered the hard way:

1. CLT shipped a `libPackageDescription.dylib` inconsistent with its own
   `.swiftmodule`, so SwiftPM could not link any manifest — even a three-line
   package failed. Reinstalling Command Line Tools fixes this and is worth doing
   first; it is what makes `swift build` work.
2. Executing the resulting `.xctest` bundle needs the `xctest` runner, which
   ships only with Xcode. XCTest is not an escape hatch — it is also Xcode-only.
   `Testing.framework` *is* present in CLT, which is why the suite uses
   swift-testing and `Package.swift` adds the CLT framework search path when it
   finds one.

`build-app.sh` needs none of this; it falls back to `swiftc` and produces the
same binary.

## Roadmap

1. ~~Delete dead code~~ — done (477 lines).
2. **Scaffold** — done. Menu bar, pill, hotkey, injection, config, permissions,
   login item. No STT.
3. **Speech recognition** — done, pending validation. whisper.cpp linked via its
   C API; VAD, adaptive noise floor and hallucination filter ported. See
   "Decision 1" above, including what remains unvalidated.
4. **Distribution** — `scripts/package.sh` produces a shareable zip today, but
   the app is signed with a local self-signed identity and is not notarized, so
   recipients need `xattr -dr com.apple.quarantine` once. Notarization needs a
   paid Apple Developer account. Still arm64-only; a universal build is a
   `CMAKE_OSX_ARCHITECTURES` change plus the matching swiftc target.
5. **Bundle Python** — embed a relocatable interpreter (python-build-standalone)
   under `Contents/Resources/python` for `whiz analyze` and friends, plus a
   `/usr/local/bin/whiz` shim. Every bundled dylib needs individual signing for
   notarization. Not started; unlike phase 3 this is not on the critical path,
   since dictation no longer needs Python at all.
6. **Cutover** — delete `service.py`, `setup.py`, and the `macos_*` providers.
   Sign with a Developer ID and notarize.
7. **Extract a `WhizKit` library target** — move everything except `@main` and
   `AppDelegate` out of the executable, leaving a thin app shell.

   Two reasons. Xcode 16 refuses to render SwiftUI previews in an executable
   target without the `ENABLE_DEBUG_DYLIB` build setting, which `Package.swift`
   has no way to express — previews only work in library targets. And a test
   target is meant to depend on a library; `@testable import` of an executable
   works but is a known rough edge.

   `#Preview` blocks already exist in `UI/IndicatorPanel.swift`,
   `UI/SettingsView.swift`, `UI/WhizLogo.swift` and `UI/AppliesNote.swift`. They
   compile and cost nothing; they start rendering the moment this lands.

   Cost is ~2,900 lines relocated and seven types made `public`
   (`SessionController`, `IndicatorPanel`, `HotkeyManager`, `SettingsWindow`,
   `MenuBarContent`, `WhizLogo`, `Log`) plus the members the shell touches.
   Mechanical, but it touches every file — worth doing as its own PR so it does
   not bury a feature change in access-modifier churn.
8. **Other platforms** — same structure for Windows and Linux; `providers/base.py`
   stays as the seam.
