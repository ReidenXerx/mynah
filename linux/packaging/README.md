# packaging/

Everything `mynah` needs installed that is not a compiled binary:

- `mynah.service` — the systemd **user** unit. The package installs it at
  `/usr/lib/systemd/user/mynah.service`; `mynah service install` wraps
  `systemctl --user` around it (enable --now), and the Omarchy plugin's
  "Start it at login" hands over from a socket-owned engine to it.
- `benchmark.wav` — the clip the Linux model tiers are benchmarked
  against (`mynah models benchmark`, and the first-start auto-run when
  `model` is unset). **The file in the tree right now is a 200 Hz
  placeholder tone**: it exists so the install path is real, but timing a
  tone measures nothing about speech. Before the AUR publish it must be
  replaced with a short real-speech recording (a few seconds, 16 kHz mono
  s16le), and the `mynah models benchmark` budgets in
  `core/src/models/tiers.hpp` re-measured against it — the starting
  budgets (gpu ≤ 2 s, small ≤ 5 s) come from the migration plan, whose
  Phase 0 measurement pass was skipped.
- `PKGBUILD` — the release package.
- `PKGBUILD-git` — the AUR `-git` variant, published first.

Build on Arch: `cd linux/packaging && makepkg -si`. The package builds
the whole tree with the pinned whisper.cpp statically linked (M6), the
Vulkan backend in (the `gpu` tier), and `GGML_NATIVE=OFF` so the package
is portable between machines of the same arch.