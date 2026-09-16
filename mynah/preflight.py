"""Guided first-time setup / doctor for ``mynah``.

``mynah setup`` is a one-command onboarding flow that gets a fresh
pipx user from ``pipx install`` to a running always-on dictation agent:

1. Auto-inject the ``dictate`` extra (``pipx inject mynah 'mynah[macos]'``)
   if the heavy deps are missing — no separate manual step.
2. Request macOS Accessibility permission (opens System Settings) and
   poll until granted — no crash loop.
3. Request macOS Microphone permission (triggers the OS prompt by briefly
   opening a sounddevice input stream).
4. Validate the configured hotkey parses.
5. Auto-install the login LaunchAgent so dictation starts at login and
   the hotkey is always armed — no second command.

Each check returns a ``CheckResult`` (ok, title, detail, hint). The flow
re-runs checks after auto-fixing the extra so the post-install state is
verified, not assumed.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass
class CheckResult:
    """Outcome of one setup check."""

    ok: bool
    title: str
    detail: str
    hint: str = ""


def _is_linux() -> bool:
    return sys.platform.startswith("linux")


def extra_name() -> str:
    """The extra that carries this platform's runtime deps."""
    return "linux" if _is_linux() else "macos"


def _required_modules() -> tuple[str, ...]:
    """What has to import before dictation can run here.

    Linux needs no pynput (the compositor owns the hotkey) and no pyobjc
    (there is no window to draw) — capture and segmentation, and that is all.
    """
    if _is_linux():
        return ("sounddevice", "webrtcvad", "numpy")
    return ("sounddevice", "pynput", "webrtcvad", "AppKit", "Quartz", "ApplicationServices")


def _extra_installed() -> bool:
    """True if this platform's extra is importable."""
    for mod in _required_modules():
        try:
            __import__(mod)
        except ImportError:
            return False
    return True


def _inject_extra() -> int:
    """Run ``pipx inject mynah 'mynah[<platform>]'``, streaming output live.

    Returns the pipx exit code (0 = success).
    """
    return subprocess.run(
        ["pipx", "inject", "mynah", f"mynah[{extra_name()}]"], check=False
    ).returncode


def _check_extra() -> CheckResult:
    """Verify the 'macos' extra's importable deps are installed."""
    if _extra_installed():
        return CheckResult(
            ok=True,
            title="Runtime extra",
            detail=", ".join(_required_modules()) + " all importable",
        )
    return CheckResult(
        ok=False,
        title="Runtime extra",
        detail="Missing deps — will auto-install now",
        hint=f"pipx inject mynah 'mynah[{extra_name()}]'",
    )


def _check_accessibility() -> CheckResult:
    """Verify macOS Accessibility permission (for CGEvent + global hotkey).

    Requests the prompt (opens System Settings) if not yet granted.
    """
    try:
        from ApplicationServices import AXIsProcessTrustedWithOptions
        from CoreFoundation import kCFBooleanTrue
        from Foundation import NSDictionary

        # The prompt option opens System Settings → Accessibility if not yet
        # trusted, so the user is taken straight to the right pane.
        options = NSDictionary.dictionaryWithDictionary_(
            {"AXTrustedCheckOptionPrompt": kCFBooleanTrue}
        )
        trusted = AXIsProcessTrustedWithOptions(options)
        if trusted:
            return CheckResult(
                ok=True,
                title="Accessibility",
                detail="Granted — mynah can type into other apps and read the hotkey",
            )
        return CheckResult(
            ok=False,
            title="Accessibility",
            detail="Not granted yet — System Settings should be open",
            hint=(
                "System Settings → Privacy & Security → Accessibility\n"
                "  Add mynah and enable it, then re-run: mynah setup\n"
                "  Note: the always-on LaunchAgent is a separate process and\n"
                "  needs its OWN grant — see `mynah service install`."
            ),
        )
    except ImportError:
        return CheckResult(
            ok=False,
            title="Accessibility",
            detail="PyObjC not installed — cannot check",
            hint=f"pipx inject mynah 'mynah[{extra_name()}]'",
        )


def _check_microphone() -> CheckResult:
    """Verify microphone permission by briefly opening an input stream.

    Opening a sounddevice InputStream is exactly what triggers macOS's
    microphone permission prompt on first use, so this check doubles as the
    prompt trigger. If permission is denied, sounddevice raises and we
    report the grant hint.
    """
    try:
        import sounddevice as sd
    except ImportError:
        return CheckResult(
            ok=False,
            title="Microphone",
            detail="sounddevice not installed — cannot check",
            hint=f"pipx inject mynah 'mynah[{extra_name()}]'",
        )
    try:
        # A zero-block, sub-second stream open is enough to trigger the OS
        # prompt and confirm the device is reachable. Keep it tiny so this
        # is fast and silent.
        with sd.InputStream(samplerate=16000, channels=1, dtype="float32", blocksize=480):
            pass
        return CheckResult(
            ok=True,
            title="Microphone",
            detail="Reachable — mic capture will work",
        )
    except Exception as e:  # noqa: BLE001
        msg = str(e).lower()
        if "input" in msg or "device" in msg or "permission" in msg or "denied" in msg:
            return CheckResult(
                ok=False,
                title="Microphone",
                detail=f"Not accessible ({e})",
                hint=(
                    "System Settings → Privacy & Security → Microphone\n"
                    "  Enable mynah (or the terminal/Python running it),\n"
                    "  then re-run: mynah setup"
                ),
            )
        return CheckResult(
            ok=False,
            title="Microphone",
            detail=f"Could not open mic ({e})",
            hint="Check that a microphone is connected and not in use by another app.",
        )


def _check_hotkey() -> CheckResult:
    """Validate the configured hotkey parses with pynput.

    A hotkey that pynput's ``HotKey.parse`` rejects (e.g. the historical
    ``<cmd>+<shift>+<period>`` default — pynput does not treat ``<period>`` as
    a named key token, the '.' key must be a literal character) makes the
    engine's hotkey listener fail to start. Under a KeepAlive LaunchAgent that
    is a silent crash loop with no way to trigger dictation — so catch it here
    at setup time, before the service is installed, with an actionable hint.
    """
    try:
        from pynput import keyboard
    except ImportError:
        # The extra check already reports a missing pynput; don't duplicate.
        return CheckResult(
            ok=False,
            title="Hotkey",
            detail="pynput not installed — cannot validate",
            hint=f"pipx inject mynah 'mynah[{extra_name()}]'",
        )
    from mynah.config import load as load_config

    cfg = load_config()
    hotkey = cfg.hotkey
    try:
        keyboard.HotKey.parse(hotkey)
        return CheckResult(
            ok=True,
            title="Hotkey",
            detail=f"Valid ({hotkey})",
        )
    except Exception as e:  # noqa: BLE001
        return CheckResult(
            ok=False,
            title="Hotkey",
            detail=f"Invalid ({hotkey}): {e}",
            hint=(
                "The hotkey must use pynput syntax. Literal keys like '.' are\n"
                "  written bare, not as '<period>'. Fix it with:\n"
                "  mynah set hotkey=\"<cmd>+<shift>+.\""
            ),
        )


# ---------------------------------------------------------------------------
# Linux checks. Nothing here is a permission dialog: on Wayland what can be
# missing is a tool, a model, or a keybinding the compositor owns.
# ---------------------------------------------------------------------------


def _check_typing() -> CheckResult:
    """Can we type into the focused window at all?"""
    from mynah.providers.linux_inject import WtypeInjector

    ok, hint = WtypeInjector().check_permissions(prompt=False)
    if ok:
        return CheckResult(
            ok=True,
            title="Typing",
            detail="wtype is installed — mynah can type into the focused window",
        )
    first, _, rest = hint.partition("\n")
    return CheckResult(ok=False, title="Typing", detail=first, hint=rest.strip())


def _check_speech() -> CheckResult:
    """whisper.cpp and a model to run through it."""
    from mynah.providers import linux_stt
    from mynah.config import load as load_config

    binary = linux_stt.find_binary()
    if binary is None:
        return CheckResult(
            ok=False,
            title="Speech",
            detail="whisper.cpp is not installed",
            hint=(
                "Arch:   sudo pacman -S whisper-cpp\n"
                "  Debian: sudo apt install whisper.cpp\n"
                "  Or point mynah at a build: MYNAH_WHISPER_CLI=/path/to/whisper-cli"
            ),
        )
    wanted = load_config().model
    model = linux_stt.find_model(wanted)
    if model is None:
        name = (wanted or linux_stt.DEFAULT_MODEL).strip()
        size = linux_stt.MODEL_SIZES.get(name, "")
        known = name if name in linux_stt.MODEL_SIZES else linux_stt.DEFAULT_MODEL
        return CheckResult(
            ok=False,
            title="Speech",
            detail=f"{Path(binary).name} is installed, but there is no {name!r} model",
            hint=(
                f"Download it{f' ({size})' if size else ''}:\n"
                f"  {linux_stt.download_command(known)}\n"
                "  Or point at one you have:  mynah set model=/path/to/ggml-small.bin"
            ),
        )
    return CheckResult(
        ok=True,
        title="Speech",
        detail=f"{Path(binary).name} with {model.name}",
    )


def _check_compositor_hotkey() -> CheckResult:
    """Is a key actually bound to `mynah toggle`?

    No client can grab a global hotkey on Wayland, so this is the compositor's
    job and usually beyond our reach. Hyprland can be asked, which is the
    common case on Omarchy — anywhere else we say what to bind and leave the
    verdict open rather than claim a check we did not make.
    """
    binds = _hyprland_binds()
    if binds is None:
        return CheckResult(
            ok=True,
            title="Hotkey",
            detail="Bound in your compositor (mynah cannot read it from here)",
            hint="Bind a key to:  mynah toggle",
        )
    for combo, dispatched, description in binds:
        # A binding made in Lua — which is how the Omarchy plugin makes ours —
        # reports its dispatcher as "__lua" and its argument as an index into
        # Hyprland's own table, so the command is not in the binding at all.
        # Its description is, which is why the plugin sets one.
        haystack = f"{dispatched} {description}".lower()
        if "mynah" in haystack:
            return CheckResult(
                ok=True,
                title="Hotkey",
                detail=f"{combo} → {description or dispatched}",
            )
    return CheckResult(
        ok=False,
        title="Hotkey",
        detail="No Hyprland binding runs mynah",
        hint=(
            "Add one, then reload:\n"
            "  bind = SUPER, D, exec, mynah toggle\n"
            "  The Omarchy plugin binds it for you: "
            "omarchy plugin add https://github.com/ReidenXerx/omarchy-mynah.git --enable"
        ),
    )


# Hyprland's modmask bits, in the order people say them.
_MODS = ((64, "SUPER"), (4, "CTRL"), (8, "ALT"), (1, "SHIFT"))


def _combo(modmask: object, key: str) -> str:
    """"SUPER+ALT+D" from a modmask and a key name."""
    if not isinstance(modmask, int):
        return key
    parts = [name for bit, name in _MODS if modmask & bit]
    parts.append(key)
    return "+".join(parts)


def _hyprland_binds() -> list[tuple[str, str, str]] | None:
    """Hyprland's bindings as (combo, dispatched, description), or None.

    None means "no verdict": not Hyprland, no hyprctl, or a version whose JSON
    we do not recognize. A check that cannot run must not fail the user.
    """
    if not os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
        return None
    hyprctl = shutil.which("hyprctl")
    if hyprctl is None:
        return None
    try:
        done = subprocess.run(
            [hyprctl, "-j", "binds"], capture_output=True, text=True, timeout=5, check=False
        )
        if done.returncode != 0:
            return None
        binds = json.loads(done.stdout)
    except (OSError, ValueError, subprocess.SubprocessError):
        return None
    if not isinstance(binds, list):
        return None
    out = []
    for bind in binds:
        if not isinstance(bind, dict):
            continue
        key = str(bind.get("key", ""))
        combo = _combo(bind.get("modmask", 0), key)
        out.append((
            combo,
            f"{bind.get('dispatcher', '')} {bind.get('arg', '')}".strip(),
            str(bind.get("description", "")),
        ))
    return out


_CHECKS = (
    ("extra", _check_extra),
    ("accessibility", _check_accessibility),
    ("microphone", _check_microphone),
    ("hotkey", _check_hotkey),
)

_CHECKS_LINUX = (
    ("extra", _check_extra),
    ("typing", _check_typing),
    ("speech", _check_speech),
    ("microphone", _check_microphone),
    ("hotkey", _check_compositor_hotkey),
)


def checks_for_platform() -> tuple[tuple[str, object], ...]:
    """The checks that mean something on this platform."""
    return _CHECKS_LINUX if _is_linux() else _CHECKS


def run_checks() -> list[CheckResult]:
    """Run all prerequisite checks in order, returning their results."""
    return [fn() for _name, fn in checks_for_platform()]


def _mark(ok: bool) -> str:
    return "✓" if ok else "✗"


def setup(install_service: bool = True) -> int:
    """One-command onboarding: inject extra → check perms → install service.

    Auto-injects the ``dictate`` extra if missing, checks Accessibility +
    Microphone permissions, validates the hotkey, and installs the always-on
    login LaunchAgent when everything passes.

    Args:
        install_service: When True (default), auto-install the LaunchAgent
            after all checks pass. Set False to just run checks without
            installing the service (the ``--no-service`` flag).

    Returns 0 if all checks pass, 1 otherwise.
    """
    print("mynah — first-time setup\n", file=sys.stderr)

    # Step 0: auto-inject the macos extra if missing. This downloads
    # mlx-whisper + deps (~1.6 GB), so stream pipx's output live. After
    # injecting, the checks import lazily so they'll pick up the fresh install.
    if not _extra_installed():
        print(f"  ⚙ Installing the {extra_name()} extra…", file=sys.stderr)
        if not _is_linux():
            print("    This downloads ~1.6 GB — give it a minute.\n", file=sys.stderr)
        rc = _inject_extra()
        if rc != 0:
            print(
                f"  ✗ Failed to install the {extra_name()} extra (pipx exit {rc}).\n"
                f"    Run manually: pipx inject mynah 'mynah[{extra_name()}]'",
                file=sys.stderr,
            )
            return 1
        print("  ✓ Runtime extra installed.\n", file=sys.stderr)

    # Run all checks (extra, accessibility, microphone, hotkey).
    results = run_checks()
    all_ok = True
    for r in results:
        all_ok = all_ok and r.ok
        print(f"  {_mark(r.ok)} {r.title} — {r.detail}", file=sys.stderr)
        if not r.ok and r.hint:
            for line in r.hint.splitlines():
                print(f"      {line}", file=sys.stderr)
    print(file=sys.stderr)

    if not all_ok:
        print(
            "Some checks failed. Fix the items above, then re-run:\n"
            "  mynah setup",
            file=sys.stderr,
        )
        return 1

    print("All checks passed — dictation is ready to run.\n", file=sys.stderr)

    # Auto-install the always-on login service unless the user opted out.
    if not install_service:
        print(
            "To install the always-on login service (dictation starts at\n"
            "login, hotkey always armed):\n"
            "  mynah service install",
            file=sys.stderr,
        )
        return 0

    from mynah import service

    if _service_loaded():
        print(
            "The always-on login service is already installed and running.\n"
            "  Manage it with: mynah service status | uninstall",
            file=sys.stderr,
        )
        return 0

    print("  ⚙ Installing the always-on login service…", file=sys.stderr)
    rc = service.install()
    if rc != 0:
        print("  ✗ Service install failed — see the message above.", file=sys.stderr)
        return 1
    print("  ✓ Service installed. Dictation starts at login.\n", file=sys.stderr)
    print(
        "Setup complete! Press Cmd+Shift+. to start dictation.\n"
        "The process shows as 'mynah' in Activity Monitor. If Accessibility\n"
        "wasn't granted yet, the service will prompt automatically and wait.",
        file=sys.stderr,
    )
    return 0


def _service_loaded() -> bool:
    """Best-effort check of whether the LaunchAgent is loaded."""
    try:
        from mynah import service

        res = service._run(["launchctl", "list", service.LABEL])
        return res.returncode == 0
    except Exception:  # noqa: BLE001
        return False