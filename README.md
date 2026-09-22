<div align="center">

<img src="docs/assets/mynah-mark.svg" alt="mynah" width="86">

# mynah

**Say it. It types where you are.**

Press a key, talk, and your words land in whatever window has focus — as you pause, not when you stop.
Everything runs on the machine you are sitting at.

[![License: MIT](https://img.shields.io/badge/license-MIT-F5B301)](LICENSE)
[![Python ≥3.11](https://img.shields.io/badge/python-%E2%89%A53.11-F5B301)](https://www.python.org/)
[![macOS](https://img.shields.io/badge/macOS-shipping-3FBF9A)](#where-it-runs)
[![Linux · Wayland](https://img.shields.io/badge/Linux%20%C2%B7%20Wayland-shipping-3FBF9A)](docs/LINUX-APP.md)

```bash
pipx install "git+https://github.com/ReidenXerx/mynah.git"
mynah setup
```

**[duduphudu.app/mynah](https://duduphudu.app/mynah/)** — what it does, and what it refuses to do

</div>

---

## Why another dictation tool

Most of them hand the whole recording to a model when you stop talking, then paste the result. Mynah
splits what you say into **utterances** and types each one the moment you pause, so text appears at
the speed of speech instead of arriving in a block.

Getting that right is the hard part, and it is where most of this project lives:

- **A tuned segmentation contract.** `tuning/tuning.toml` holds the thresholds — how long a silence
  ends an utterance, how much trailing silence to trim, how loud speech has to be over the room. They
  are not guesses; they are pinned by a **golden corpus** of recordings in `tuning/golden/`, and the
  test suite fails if a change moves a single boundary.
- **Calibration that cannot backfire.** The noise floor is measured at the start of a session, but its
  contribution is capped: a calibrated gate above the speech line would demand speech louder than
  speech, and no setting could counter it. Frames captured during calibration are kept, not dropped —
  if you start talking immediately, that speech is still segmented.
- **Gates you can move.** Too sensitive in a café, not sensitive enough in a quiet room:
  `mynah set sensitivity=0.005` and try again.

## What a session looks like

```
$ mynah                              # or let the login service run it

  ●  idle          the hotkey is armed, nothing is listening
  ●  listening     you pressed it — the pill shows your level
  ●  transcribing  you paused; this utterance is going into the app
  ●  listening     still your turn
```

The indicator is a small pill, the menu bar or tray item carries start, stop and quit, and the model
stays warm for 45 seconds after you finish so back-to-back dictation is instant. After that it
unloads and Mynah costs nothing at all.

## Install

**macOS**

```bash
pipx install "git+https://github.com/ReidenXerx/mynah.git"
pipx inject mynah "mynah[macos] @ git+https://github.com/ReidenXerx/mynah.git"
mynah setup                          # dependencies, permissions, hotkey, login service
```

**Linux (Wayland)**

```bash
MYNAH=<the commit you are installing>   # a full 40-character SHA, never a branch name

# The engine at that exact commit, with nothing resolved from PyPI yet.
pipx install --pip-args="--no-deps" "git+https://github.com/ReidenXerx/mynah.git@$MYNAH"

# What it imports, at pinned versions, every artifact checked against its hash.
pipx runpip mynah install --require-hashes --only-binary :all: \
    --no-binary webrtcvad-wheels --no-build-isolation \
    -r https://raw.githubusercontent.com/ReidenXerx/mynah/$MYNAH/requirements/build.lock
pipx runpip mynah install --require-hashes --only-binary :all: \
    --no-binary webrtcvad-wheels --no-build-isolation \
    -r https://raw.githubusercontent.com/ReidenXerx/mynah/$MYNAH/requirements/linux.lock

sudo pacman -S whisper-cpp wtype wl-clipboard   # speech, typing, and pasting
mynah setup                          # checks each of these and names what is missing
```

`--require-hashes` makes pip refuse anything not named in the lock, so nothing that runs here is
decided by PyPI after you read this. `requirements/linux.lock` covers CPython 3.11 to 3.14 on
x86_64 and aarch64, glibc and musl; `tools/lock.py` writes it, and `tools/lock.py --check` says
whether it is still current. One package is built from source rather than installed as a wheel:
webrtcvad-wheels publishes no wheel for CPython 3.14, which is what Arch ships. Its source archive
is hash-checked like everything else and builds against the pinned setuptools, with pip's build
isolation off so no unchecked build backend can be fetched in its place.

`mynah setup` is the honest path: it checks each requirement, says which one is missing and what to
do about it — including the one `curl` that downloads a speech model — and only installs the login
service once everything else passes.

On Omarchy, [omarchy-mynah](https://github.com/ReidenXerx/omarchy-mynah) adds the desktop half: it
binds the key, puts the bird in the bar with a live level, and shows a pill while it listens.

## Commands

```bash
mynah                    # start dictating; the hotkey opens and closes a session
mynah setup              # first run: dependencies, permissions, hotkey, service
mynah config             # what it is set to, and where that file is
mynah set hotkey=<f8>    # change one setting
mynah service status     # install | uninstall | status of the login service
mynah providers          # what can listen, type and show an indicator here
```

Settings live in `~/.config/mynah/config.toml`, shared with the macOS app. Useful ones:

| Setting | What it does |
| --- | --- |
| `hotkey` | The global key, in pynput syntax (`<ctrl>+<space>`, `<f8>`). macOS only — on Wayland your compositor binds `mynah toggle` |
| `trigger` | `toggle` — press to start and stop — or `ptt`, hold to talk |
| `language` | Spoken language code |
| `model` | Speech model repo or path; empty means the provider's default |
| `vad` | Split speech into utterances. Off means text only arrives at the end |
| `frame_energy` | Per-frame speech floor: lower hears more, and more of the room |
| `auto_stop_silence` | Seconds of silence that end a session by themselves |

## Where it runs

| | macOS | Linux |
| --- | --- | --- |
| Dictation engine, segmentation, tuning | ✅ | ✅ |
| Speech | mlx-whisper on the Apple GPU | whisper.cpp |
| Typing into the focused window | Accessibility API | `wtype`, and the clipboard for apps that ignore it |
| Hotkey | pynput | the compositor's own binding, to `mynah toggle` |
| Indicator and menu | native pill + menu bar item | the shell's, through `mynah watch` — a bar widget and a pill on Omarchy |
| Runs at login | `SMAppService` / LaunchAgent | `systemd --user`, tied to the graphical session |
| Native app | `Mynah.app`, no Python at runtime — see [SWIFT-APP.md](docs/SWIFT-APP.md) | the desktop's own shell — see [LINUX-APP.md](docs/LINUX-APP.md) |

The engine is provider-abstracted: speech, typing and indicator are interfaces, and a platform is a
set of implementations. That is why the Linux port is three small providers rather than a rewrite —
and why the tuning contract is shared instead of re-derived.

Wayland only on Linux, deliberately: on X11 any client can already read the keyboard and inject
keystrokes, so there is nothing to grant and nothing to revoke. GNOME is the gap — it does not
implement the virtual-keyboard protocol `wtype` needs, and a portal injector is not written yet.

## Nothing leaves your machine

Speech is transcribed locally — mlx-whisper on macOS, whisper.cpp elsewhere. There is no account, no
API key and no endpoint to disable, because there is none to begin with. The only file Mynah writes
outside its own config is the text it types, into the window you were already in.

> **The name `mynah` on PyPI is not this project.** It belongs to an unrelated
> package, so the extras are always requested from this repository —
> `mynah[linux] @ git+…` — and never as a bare `mynah[linux]`, which pip would
> fetch from PyPI. `mynah setup` does the same thing for you.

## It came out of whiz

Mynah was `whiz dictate` until it outgrew it. [whiz](https://github.com/ReidenXerx/whiz) turns a
recording into a transcript; Mynah types while you talk. Two different jobs that happened to share a
speech engine, so they now share nothing but their maker.

Your old settings are not lost: the first run imports the `dictate_*` keys from
`~/.config/whiz/config.toml` if they are there.

## Tests

```bash
pip install pytest && python -m pytest
```

197 of them, and the ones that matter most are the golden-corpus tests: recordings with known
boundaries, asserted against the segmentation contract.

## License

MIT — see [LICENSE](LICENSE).
