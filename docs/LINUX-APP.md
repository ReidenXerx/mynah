# Dictation on Linux

The same engine as macOS, with the edges turned outward.

**State: written, not yet run.** Every piece below exists and its tests pass —
on a Mac. Nothing in `linux/` has been compiled on Linux, no AUR package is
published, and the Omarchy plugin acceptance run has not happened. What ships
today is still the retired Python engine, from the commit the plugin pins. Read
this as the design and the plan, not as a description of something working.

On macOS the app owns everything: it registers its own hotkey, types with the
Accessibility API and draws its own panel. On Wayland it can do none of those,
and that is not a gap to work around — it is the design of the platform. A
client may not read the keyboard globally, and the shell, not the app, draws the
desktop.

So on Linux the engine is a headless binary plus one socket, and the desktop
supplies the rest:

| Concern | Here | Why |
|---|---|---|
| Engine | `libmynah`, linked in | the same segmentation, VAD, whisper and filter as the Mac app — one implementation, not a port |
| Speech | whisper.cpp **in-process**, model kept warm | the Python engine spawned `whisper-cli` per utterance and reloaded the model each time; in-process removes both, and the audio never touches a file |
| Typing | `wtype`, text on **stdin**; the clipboard where that fails | the virtual-keyboard protocol is the compositor-blessed way to synthesize input — where it is honoured |
| Indicator | published on the control socket | the shell draws it, in the desktop's own idiom |
| Hotkey | your compositor, bound to `mynah toggle` | no Wayland client may grab a global key |
| Capture | PipeWire directly, 16 kHz mono float | what Omarchy and Plasma both run; no PortAudio layer to keep compatible |
| Segmentation | the core's | the golden corpus pins one implementation for every front end |
| Service | `systemd --user`, tied to `graphical-session.target` | the Linux equivalent of the LaunchAgent |

Build it (there is no package yet):

```bash
git clone --recurse-submodules https://github.com/ReidenXerx/mynah.git
cd mynah
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/linux/mynah setup       # checks typing, speech, microphone, hotkey
```

Needs Node-free but not dependency-free: `pipewire`, `wtype`, `wl-clipboard`,
`curl`, `openssl`, and for the `gpu` tier `vulkan-icd-loader` (plus
`vulkan-headers` and `shaderc` to build it). `linux/packaging/` carries the
PKGBUILDs and the systemd unit for when this is packaged.

`mynah setup` checks each requirement, names what is missing, and prints the
command that fixes it — including `mynah models download small`.

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

(Synthetic speech, which is harder than a real voice.) **Measured against the
Python engine**, which spawned `whisper-cli` per utterance and reloaded the
model every time; the binary keeps the model warm, so these numbers are an
upper bound it should beat — by how much is unmeasured, because nothing has run
on Linux yet. Utterances pipeline — the next is captured while the last is
transcribed — but talk faster than your machine transcribes and the text falls
further behind.

Which model to run is no longer a setting you are expected to guess. The tiers
(`gpu` → turbo, `small`, `base`) are chosen by `mynah models benchmark`, which
times each candidate on a bundled clip and picks the largest that meets a
latency budget; `mynah set model=…` still overrides. **The budgets are
placeholders** from the migration plan, and the bundled clip is still a tone
rather than speech — both wait on the measurement pass (Phase 0).

The `gpu` tier builds the Vulkan backend into the package. An integrated GPU is
used automatically; a discrete one only when you set `gpu = true`, because
waking a dGPU for every sentence is the machine owner's choice, not a dictation
tool's.

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
