# Dictation on Linux

The same engine as macOS, with the edges turned outward.

On macOS mynah owns everything: it grabs its own hotkey with pynput, transcribes
in-process with mlx-whisper, types with the Accessibility API and draws its own
NSPanel. On Wayland it can do none of those things, and that is not a gap to
work around — it is the design of the platform. A client may not read the
keyboard globally, and the shell, not the app, draws the desktop.

So the Linux side is a set of providers plus one socket:

| Concern | Here | Why |
|---|---|---|
| Speech | `whisper-cli` (whisper.cpp), a subprocess per utterance | the binary the distro already packages; no Python speech dependency at all |
| Typing | `wtype`, text on **stdin**; the clipboard where that fails | the virtual-keyboard protocol is the compositor-blessed way to synthesize input — where it is honoured |
| Indicator | published on the control socket | the shell draws it, in the desktop's own idiom |
| Hotkey | your compositor, bound to `mynah toggle` | no Wayland client may grab a global key |
| Capture | `sounddevice`, 16 kHz mono, 30 ms frames | unchanged from macOS; PipeWire serves it through PortAudio |
| Segmentation | unchanged | the golden corpus pins one implementation for both platforms |
| Service | `systemd --user`, tied to `graphical-session.target` | the Linux equivalent of the LaunchAgent |

Install:

```bash
pipx install "git+https://github.com/ReidenXerx/mynah.git"
# The extra comes from this repository: the bare name `mynah` on PyPI is an
# unrelated package, and pip would happily fetch that instead.
pipx inject mynah "mynah[linux] @ git+https://github.com/ReidenXerx/mynah.git"
sudo pacman -S whisper-cpp wtype wl-clipboard
mynah setup
```

`mynah setup` checks each of those, names what is missing, and prints the exact
command that fixes it — including the `curl` that downloads a model.

## The control socket

`$XDG_RUNTIME_DIR/mynah/control.sock`, line-delimited JSON, mode 0600 in a 0700
directory the server refuses to use unless it owns it.

```
{"cmd": "toggle"}     -> {"ok": true, "state": "idle"}
{"cmd": "status"}     -> {"ok": true, "state": "listening", "pid": 4242}
{"cmd": "subscribe"}  -> then one event per line:
                         {"event": "state", "state": "listening"}
                         {"event": "level", "level": 0.42,
                          "bands": [0.71, 1.0, 0.78, ...]}
                         {"event": "text",  "text": "the fee is computed"}
```

`mynah toggle` is what a keybinding runs. `mynah watch` is what a shell plugin
reads: every line is a complete JSON object and stdout is flushed per line, so a
`Process` with a line parser is the whole integration.

Two properties that are easy to get wrong and hard to notice:

- **A command is acknowledged, then carried out.** Ending a session drains the
  transcription queue, which is seconds of whisper. Replying only afterwards
  made the key that stopped dictation look wedged and timed the client out.
- **Level events are coalesced to ~30 a second.** They are published from the
  capture thread for every 30 ms frame, and a shell that stops reading must
  never be able to stall audio.
- **`bands` is the frame's spectrum**, twelve log-spaced bands from 80 Hz to
  5 kHz — where a voice lives — measured by a 480-sample FFT and normalised by
  the window's own sum, so a band's value is in amplitude units rather than raw
  FFT magnitudes. The engine only computes it when the selected indicator sets
  `wants_spectrum`, and it rides with the level rather than as its own event.
  A subscriber that only wants one number ignores it.

## Apps that ignore the virtual keyboard

`wtype` uploads its own keymap and then sends keycodes from it. An app that
never applies that keymap reads those keycodes with the layout the real
keyboard has — and since the generated keymap assigns characters to keycodes
from 1 upward, `echo plain abcdef` arrives as `1234567894701-=`. Warp does
this. Nothing in the protocol lets us detect it; the app just renders digits.

So the default injector is `smart`: it types with `wtype`, and for the apps
known to do this it puts the utterance on the clipboard and asks the
**compositor** to deliver the paste chord (`hl.dsp.send_shortcut` on Hyprland —
not a virtual keyboard, which is the thing being ignored). Terminals get
Ctrl+Shift+V, everything else Ctrl+V, and the clipboard is put back about a
second later. A clipboard holding an image is left untouched rather than
replaced with text.

`mynah set injector=wtype` never pastes; `injector=clipboard` always does.

## The desktop half

[omarchy-mynah](https://github.com/ReidenXerx/omarchy-mynah) is the Omarchy
plugin: it binds the key through `hl.bind` (and re-binds after a config reload,
which drops runtime binds), puts the bird in the bar with a live level, and
shows a pill while a session is open. It starts the engine itself when nothing
else is running one, and adopts the systemd service when there is one.

Nothing about the engine depends on it. Without a shell plugin, dictation works
exactly the same; it just has no face.

## Latency, measured

Whisper encodes a 30-second window whatever you said, so the cost is per
utterance, not per second of speech. On a 22-core Meteor Lake laptop, CPU only:

| Model | Per utterance | "The fee is computed on gross, but it should be net." |
|---|---|---|
| `ggml-base` (142 MB) | ~1.5 s | "The **feed** is computed on **bros**, but it should be net." |
| `ggml-small` (466 MB) | ~4.2 s | exact |

(Synthetic speech, which is harder than a real voice.) Utterances pipeline — the
next is captured while the last is transcribed — but talk faster than your
machine transcribes and the text falls further behind. `small` is the default
because a wrong word costs more than a second; `mynah set model=base` is there
for a slower machine or a faster feel.

A GPU backend would change this picture: whisper.cpp loads `ggml-*` backends
from `/usr/lib/ggml`, and on Arch `ggml-cuda`, `ggml-hip` and `ggml-openvino` are
packaged. None is installed by default, and mynah does not install one: waking a
discrete GPU for every sentence is a choice for the machine's owner, not for a
dictation tool.

## Wayland only

No X11 path, deliberately. On X11 any client can read the keyboard and inject
keystrokes globally, which is exactly what dictation needs — so there is nothing
to grant and nothing to revoke, and a permission story there would be theatre.
Wayland puts both capabilities where they can be seen: the compositor decides
whether to honour the virtual keyboard, and the binding is the user's own.

`wtype` needs `virtual-keyboard-v1`, which wlroots compositors (Hyprland, Sway,
river) implement. GNOME does not, and there `wtype` fails — a `RemoteDesktop`
portal injector is the way in, and is not written yet. That is the honest state
of the support matrix:

| Desktop | Hotkey | Typing |
|---|---|---|
| Hyprland / Sway / wlroots | your compositor's own binding | ✅ `wtype` |
| KDE Plasma (Wayland) | its global shortcuts | ✅ `wtype` (KWin implements the protocol) |
| GNOME (Wayland) | its custom shortcuts | ❌ needs a portal injector |
| X11, any desktop | — | ❌ by decision, see above |

## What a Rust daemon would buy

An earlier plan for this was a Rust daemon (`mynah-core`) with PipeWire capture,
the GlobalShortcuts portal and a StatusNotifierItem tray. It is not what shipped,
because the engine already existed and the platform-specific part turned out to
be three small providers rather than a port.

What that plan would still buy, if the Python version's costs ever bite: no
interpreter in the session, no per-utterance subprocess, and a tray that works on
desktops with no Quickshell. Nothing about the current design blocks it — the
control protocol above is the contract, and a Rust engine that speaks it would
leave the Omarchy plugin untouched.
