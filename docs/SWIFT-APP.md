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
| Model resolution | `STT/WhisperModel.swift` | `providers/mlx.py` |
| Gates + hallucination filter | `STT/TranscriptFilter.swift` | `engine.py` constants |
| Utterance segmentation | `STT/UtteranceDetector.swift` | `vad.py` + `engine.py` |
| Mic capture (16 kHz mono) | `Session/AudioCapture.swift` | `sounddevice` |

Speech recognition is wired (see "Decision 1"), but **has never been run against
real speech** — only against synthetic noise, and never with the UI on screen.
Treat the end-to-end path as unverified until someone dictates into it.

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

1. **Homebrew's dylibs are built for macOS 26**, so linking them contradicts the
   macOS 13 deployment floor (`ld: building for macOS-13.0, but linking with
   dylib ... built for newer version 26.0`). Fine for local development; a
   distributable app must vendor and build whisper.cpp with its own deployment
   target rather than link Homebrew's.
2. **ggml backends need explicit registration.** Metal, BLAS and CPU ship as
   loadable modules in `$(brew --prefix)/Cellar/ggml/*/libexec/*.so` and are not
   registered automatically. `WhisperEngine` calls `ggml_backend_load_all()`
   first; without it the device registry is empty.
3. **whisper aborts rather than returning null** when no backend device exists —
   `GGML_ASSERT(device)` calls `abort()` inside
   `whisper_init_from_file_with_params`, killing the process. It cannot be
   caught, so the backend registration in issue 2 is load-bearing, not
   defensive.
4. **Homebrew version skew.** `whisper-cli` links ggml 0.18.1 while the installed
   ggml is 0.20.2. Another argument for vendoring.
5. **First-ever model load takes ~7 s** compiling the Metal library, versus
   ~0.25 s once cached. Cold start needs UI feedback or a warm-up at launch.
6. **Only `ggml-large-v3-turbo-q5_0.bin` is on this machine.** The preferred
   unquantized turbo is not downloaded, so `WhisperModel.resolve` currently falls
   through to the quantized model that `ea49da8` warns about. Run
   `whiz models download ggml-large-v3-turbo.bin` before quality testing, or the
   test measures the wrong model.
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
9. **`models.py:PREFERENCE` prefers `q5_0` for batch.** Same model class
   `ea49da8` found garbles Russian in dictation. q5_0 is milder than q4 and may
   be fine, but the batch default now contradicts the dictation finding and
   nobody has checked. Independent of this decision, worth testing.

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

`swift test` needs SwiftPM, so `Tests/WhizAppTests` cannot run until the
toolchain is fixed.

## Roadmap

1. ~~Delete dead code~~ — done (477 lines).
2. **Scaffold** — done. Menu bar, pill, hotkey, injection, config, permissions,
   login item. No STT.
3. **Speech recognition** — done, pending validation. whisper.cpp linked via its
   C API; VAD, adaptive noise floor and hallucination filter ported. See
   "Decision 1" above, including what remains unvalidated.
4. **Bundle Python** — embed a relocatable interpreter (python-build-standalone)
   under `Contents/Resources/python` for `whiz analyze` and friends, plus a
   `/usr/local/bin/whiz` shim. Every bundled dylib needs individual signing for
   notarization.
5. **Cutover** — delete `service.py`, `setup.py`, and the `macos_*` providers.
   Sign with a Developer ID and notarize.
6. **Other platforms** — same structure for Windows and Linux; `providers/base.py`
   stays as the seam.
