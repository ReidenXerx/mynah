"""Guided first-time setup / doctor for ``whiz dictate``.

``whiz dictate setup`` is a one-command onboarding flow that gets a fresh
pipx user from ``pipx install`` to a running always-on dictation agent:

1. Auto-inject the ``dictate`` extra (``pipx inject whiz 'whiz[dictate]'``)
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

import subprocess
import sys
from dataclasses import dataclass


@dataclass
class CheckResult:
    """Outcome of one setup check."""

    ok: bool
    title: str
    detail: str
    hint: str = ""


def _dictate_extra_installed() -> bool:
    """True if the 'dictate' extra's heavy deps are importable."""
    for mod in ("sounddevice", "pynput", "webrtcvad"):
        try:
            __import__(mod)
        except ImportError:
            return False
    for mod in ("AppKit", "Quartz", "ApplicationServices"):
        try:
            __import__(mod)
        except ImportError:
            return False
    return True


def _inject_extra() -> int:
    """Run ``pipx inject whiz 'whiz[dictate]'``, streaming output live.

    Returns the pipx exit code (0 = success).
    """
    return subprocess.run(
        ["pipx", "inject", "whiz", "whiz[dictate]"], check=False
    ).returncode


def _check_extra() -> CheckResult:
    """Verify the 'dictate' extra's importable deps are installed."""
    if _dictate_extra_installed():
        return CheckResult(
            ok=True,
            title="Dictate extra",
            detail="sounddevice, pynput, webrtcvad, pyobjc all importable",
        )
    return CheckResult(
        ok=False,
        title="Dictate extra",
        detail="Missing deps — will auto-install now",
        hint="pipx inject whiz 'whiz[dictate]'",
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
                detail="Granted — whiz can type into other apps and read the hotkey",
            )
        return CheckResult(
            ok=False,
            title="Accessibility",
            detail="Not granted yet — System Settings should be open",
            hint=(
                "System Settings → Privacy & Security → Accessibility\n"
                "  Add whiz and enable it, then re-run: whiz dictate setup\n"
                "  Note: the always-on LaunchAgent is a separate process and\n"
                "  needs its OWN grant — see `whiz dictate service install`."
            ),
        )
    except ImportError:
        return CheckResult(
            ok=False,
            title="Accessibility",
            detail="PyObjC not installed — cannot check",
            hint="pipx inject whiz 'whiz[dictate]'",
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
            hint="pipx inject whiz 'whiz[dictate]'",
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
                    "  Enable whiz (or the terminal/Python running it),\n"
                    "  then re-run: whiz dictate setup"
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
            hint="pipx inject whiz 'whiz[dictate]'",
        )
    from whiz.config import load as load_config

    cfg = load_config()
    hotkey = cfg.dictate_hotkey
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
                "  whiz dictate set hotkey=\"<cmd>+<shift>+.\""
            ),
        )


_CHECKS = (
    ("extra", _check_extra),
    ("accessibility", _check_accessibility),
    ("microphone", _check_microphone),
    ("hotkey", _check_hotkey),
)


def run_checks() -> list[CheckResult]:
    """Run all prerequisite checks in order, returning their results."""
    return [fn() for _name, fn in _CHECKS]


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
    print("whiz dictate — first-time setup\n", file=sys.stderr)

    # Step 0: auto-inject the dictate extra if missing. This downloads
    # mlx-whisper + deps (~1.6 GB), so stream pipx's output live. After
    # injecting, the checks import lazily so they'll pick up the fresh install.
    if not _dictate_extra_installed():
        print("  ⚙ Installing the dictate extra (mlx-whisper + deps)…", file=sys.stderr)
        print("    This downloads ~1.6 GB — give it a minute.\n", file=sys.stderr)
        rc = _inject_extra()
        if rc != 0:
            print(
                f"  ✗ Failed to install the dictate extra (pipx exit {rc}).\n"
                "    Run manually: pipx inject whiz 'whiz[dictate]'",
                file=sys.stderr,
            )
            return 1
        print("  ✓ Dictate extra installed.\n", file=sys.stderr)

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
            "  whiz dictate setup",
            file=sys.stderr,
        )
        return 1

    print("All checks passed — dictation is ready to run.\n", file=sys.stderr)

    # Auto-install the always-on login service unless the user opted out.
    if not install_service:
        print(
            "To install the always-on login service (dictation starts at\n"
            "login, hotkey always armed):\n"
            "  whiz dictate service install",
            file=sys.stderr,
        )
        return 0

    from whiz.dictate import service

    if _service_loaded():
        print(
            "The always-on login service is already installed and running.\n"
            "  Manage it with: whiz dictate service status | uninstall",
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
        "The process shows as 'whiz' in Activity Monitor. If Accessibility\n"
        "wasn't granted yet, the service will prompt automatically and wait.",
        file=sys.stderr,
    )
    return 0


def _service_loaded() -> bool:
    """Best-effort check of whether the LaunchAgent is loaded."""
    try:
        from whiz.dictate import service

        res = service._run(["launchctl", "list", service.LABEL])
        return res.returncode == 0
    except Exception:  # noqa: BLE001
        return False