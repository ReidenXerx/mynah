# The Linux dictation adapter

`whiz-daemon` (P1 in docs/ARCHITECTURE.md) brings native dictation to Linux:
the same segmentation pipeline, pinned to the same tuning contract and golden
corpus, in a small daemon instead of a port of the PyObjC daemon.

## Decisions (settled)

**Wayland only.** No X11 support, not even as a fallback. Rationale:

- X11 has no security model: any client can snoop and inject keystrokes
  globally. Dictation needs exactly those two capabilities — reading a global
  hotkey and injecting text — so on X11 the app would be no more privileged
  than any keylogger, and any "security" prompting would be theater.
- Wayland's portals grant both capabilities with user-visible, revocable,
  per-app permission. The Linux desktop direction is Wayland (GNOME, KDE,
  and wlroots compositors all default to it), and supporting X11 doubles the
  integration surface for a security story we cannot honestly tell.
- Note: the portal APIs themselves often have X11 backends. We are not
  restricting the build to Wayland-only systems; we simply do not ship an
  X11-specific path. If a user runs a Wayland compositor with XWayland,
  portals still work for the Wayland side; dictating into X11 apps happens
  through the portal's injection, where available.

**Stack.**

| Concern | Choice | Why |
|---|---|---|
| Language | Rust | no GC pauses on the audio thread; one core crate shared with the UI (P3) |
| Audio capture | PipeWire | the Linux audio layer; wireplumber handles device routing; captures at 16 kHz mono like `sounddevice` does |
| Global shortcut | xdg-desktop-portal GlobalShortcuts | the Wayland-blessed way to get a global hotkey with a permission dialog |
| Screen/keyboard capture for injection | xdg-desktop-portal RemoteDesktop | the sanctioned input-injection path on Wayland |
| Tray | StatusNotifierItem (SNI) over D-Bus | works on GNOME (appindicator ext), KDE, wlroots trays; no GTK/Qt dependency needed to serve it |
| Service | systemd `--user` unit | the Linux equivalent of the macOS LaunchAgent: starts at login, restarts on failure, `systemctl --user` controls it |

## What the daemon does

The lifecycle mirrors the macOS app: a resident tray daemon that is idle
until the portal-registered hotkey starts a session. Per session:

1. Calibrate ambient noise for `noise_calibration_seconds` (tuning.toml),
   keeping the frames (the PR #1 lesson — speech in the window is segmented,
   not dropped).
2. Capture 16 kHz mono via PipeWire in 30 ms frames.
3. Run the energy-gate state machine (`whiz-core`'s detector — the same
   logic the golden corpus pins).
4. Close utterances at `utterance_silence`, trim trailing silence to
   `trailing_padding` (following the Swift policy — see divergences in
   ARCHITECTURE.md).
5. Gate on min-length and whole-buffer RMS, transcribe with whisper.cpp
   (vendored build, same pinned version as macOS), and inject via the
   RemoteDesktop portal session.

The hotkey toggles a session (matching `dictate_trigger = "toggle"` default);
push-to-talk arrives with the same trigger handling as macOS.

## Permissions UX

Portals put the permission story in the desktop's hands, which is what we
want on Linux (unlike X11, where nothing can be enforced):

- **GlobalShortcuts**: the compositor shows a bind/listen dialog; the user
  confirms the shortcut once, and it can be revoked per-app in settings.
- **RemoteDesktop**: a session-scoped dialog for input injection. We ask at
  first session start, not at daemon startup, so the prompt is attached to
  the user's action, not to login.

Failure modes must degrade the way the macOS app does: a denied portal
request surfaces in the tray (tooltip/menu line) and does not crash the
daemon; the daemon retries the permission on the next session attempt.

## Support matrix

| Desktop | Global hotkey | Text injection | Notes |
|---|---|---|---|
| GNOME (Wayland) | ✅ GlobalShortcuts | ✅ RemoteDesktop | XDG portal support built in |
| KDE Plasma (Wayland) | ✅ GlobalShortcuts | ✅ RemoteDesktop | native portal frontends |
| Sway / Hyprland / wlroots | ⚠️ compositor-dependent | ⚠️ compositor-dependent | portals need xdg-desktop-portal-wlr; some compositors ship their own |
| X11 sessions (any DE) | ❌ no X11 path | ❌ no X11 path | by decision — see above |

The P1 acceptance bar is the first row: GNOME or KDE on Wayland,
end-to-end dictate, golden corpus green through `whiz-core`'s detector.

## Open issues

1. **RemoteDesktop is heavy for text injection.** The portal was designed for
   screen sharing + remote control; we only need CreateVirtualKeyboard. Watch
   the spec for a lighter surface; if injection via ydotool-style uinput is
   ever sanctioned for sandboxed apps, prefer it.
2. **SNI without a full toolkit.** Serving SNI over D-Bus directly avoids
   GTK/Qt deps, but icon handling (menus, activation) is fiddly. If it costs
   more than it saves, a minimal `ksni`-style crate is the fallback.
3. **PipeWire capture permissions.** Screen-capture portals gate screen
   audio; microphone capture is Flatpak/snap sandbox policy. For a
   non-sandboxed build this is plain PipeWire permission, but if we ever
   sandbox the daemon, mic access needs the pipewire portal story nailed
   down.
4. **Calibration defect — resolved, no longer open.** The shared
   poisoned-calibration defect is fixed speech-aware in both engines
   (see ARCHITECTURE.md): calibration frames at or above
   `calibration_speech_floor` are excluded from the noise median, and
   fewer than `noise_min_samples` quiet frames aborts calibration to
   the static gates. `whiz-core` ports that exactly; the corpus cases
   `speech_during_calibration` and `speech_over_noise_in_calibration`
   pin it for every implementation, Rust included.

## Out of scope for P1

- X11 (permanent decision, see above).
- Diarization/analyze (that is P2 — this daemon is dictation only).
- UI (P3; the daemon exposes enough D-Bus surface for a tray, which
  satisfies P1 without a separate UI process).
- Model downloads: P1 uses the same `whiz models download` layout and the
  user's existing models dir; a first-class Linux downloader can come later.