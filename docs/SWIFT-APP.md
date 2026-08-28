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

**Speech recognition is deliberately absent.** `SessionController.transcribeAndInject`
is the seam and documents the steps to port. Until it lands, the hotkey starts a
session, the pill appears, and the waveform tracks your voice — but nothing is
transcribed or typed.

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

SwiftPM emits a bare executable; the script wraps it into `Whiz.app` with
`Info.plist`. Run the bundle, not the raw binary — TCC keys permissions to the
bundle identifier, so the loose executable re-prompts for Accessibility on every
rebuild.

### Known environment issue

SwiftPM does not currently work with Command Line Tools alone on this machine —
even a three-line package fails to link its manifest against
`libPackageDescription`. Full Xcode fixes it:

```sh
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

Until then the sources can still be type-checked directly, which is how the
scaffold was verified:

```sh
swiftc -sdk "$(xcrun --show-sdk-path)" -target arm64-apple-macosx13.0 \
  -swift-version 6 -parse-as-library \
  $(find macos/Sources/WhizApp -name '*.swift') -o /tmp/WhizApp
```

`swift test` needs SwiftPM, so `Tests/WhizAppTests` cannot run until the
toolchain is fixed.

## Roadmap

1. ~~Delete dead code~~ — done (477 lines).
2. **Scaffold** — done. Menu bar, pill, hotkey, injection, config, permissions,
   login item. No STT.
3. **Speech recognition** — link whisper.cpp via its C API. Port the VAD, the
   adaptive noise floor, and the hallucination filter from `engine.py`; those
   constants are tuned, not boilerplate.
4. **Bundle Python** — embed a relocatable interpreter (python-build-standalone)
   under `Contents/Resources/python` for `whiz analyze` and friends, plus a
   `/usr/local/bin/whiz` shim. Every bundled dylib needs individual signing for
   notarization.
5. **Cutover** — delete `service.py`, `setup.py`, and the `macos_*` providers.
   Sign with a Developer ID and notarize.
6. **Other platforms** — same structure for Windows and Linux; `providers/base.py`
   stays as the seam.
