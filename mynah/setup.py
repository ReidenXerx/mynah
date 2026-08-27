"""Guided first-time setup / doctor for ``whiz dictate``.

``whiz dictate setup`` walks a fresh pipx user through every prerequisite
in one command and prints a clear ✓/✗ report with next-step hints, so
onboarding isn't a scavenger hunt across the README:

1. The ``dictate`` extra installed (sounddevice, pynput, webrtcvad, pyobjc).
2. macOS Accessibility permission (for the global hotkey + text injection).
3. macOS Microphone permission (for audio capture) — probed by briefly
   opening a sounddevice input stream, the same path that triggers the OS
   permission prompt.
4. The login LaunchAgent — offered at the end once the above pass, so the
   user can install the always-on service without remembering a second
   command.

Each check returns a ``CheckResult`` (ok, title, detail, hint). The flow is
non-fatal: a ✗ prints the hint and continues to the next check rather than
aborting, so the user sees the full picture in one run. After granting a
permission, re-run ``whiz dictate setup`` to re-check.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass


@dataclass
class CheckResult:
    """Outcome of one setup check."""

    ok: bool
    title: str
    detail: str
    hint: str = ""


def _check_extra() -> CheckResult:
    """Verify the 'dictate' extra's importable deps are installed."""
    missing: list[str] = []
    for mod in ("sounddevice", "pynput", "webrtcvad"):
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    # pyobjc is imported by submodule; check the ones the injector/indicator use.
    for mod in ("AppKit", "Quartz", "ApplicationServices"):
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)
    if missing:
        return CheckResult(
            ok=False,
            title="Dictate extra",
            detail=f"Missing: {', '.join(missing)}",
            hint=(
                "Install the extra into whiz's environment:\n"
                "  pipx inject whiz 'whiz[dictate]'"
            ),
        )
    return CheckResult(
        ok=True,
        title="Dictate extra",
        detail="sounddevice, pynput, webrtcvad, pyobjc all importable",
    )


def _check_accessibility() -> CheckResult:
    """Verify macOS Accessibility permission (for CGEvent + global hotkey).

    NOTE: this checks the process running ``whiz dictate setup`` — typically
    a terminal or Python interpreter. The always-on LaunchAgent is a SEPARATE
    process and needs its OWN Accessibility grant; this check cannot verify
    that. The ``service install`` step prints a reminder, and ``service
    status`` exposes the agent's last-exit code so a missing agent grant is
    visible (exit 1 with an Accessibility message in the log).
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
            detail="Not granted yet",
            hint=(
                "System Settings → Privacy & Security → Accessibility\n"
                "  Add whiz (or the terminal/Python running it) and enable it,\n"
                "  then re-run: whiz dictate setup\n"
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


def setup() -> int:
    """Run the guided setup: checks → report → optional service install.

    Returns 0 if all checks pass, 1 otherwise.
    """
    print("whiz dictate — first-time setup\n", file=sys.stderr)
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

    # Offer the always-on login service once prerequisites are met.
    from whiz.dictate import service

    loaded = _service_loaded()
    if loaded:
        print(
            "The always-on login service is already installed and running.\n"
            "  Manage it with: whiz dictate service status | uninstall",
            file=sys.stderr,
        )
        return 0

    print(
        "Optional: install the always-on login service so dictation starts\n"
        "at login and the hotkey is always armed (no terminal needed)?\n"
        "  → whiz dictate service install",
        file=sys.stderr,
    )
    # Don't auto-install — the user may just want to run `whiz dictate` in a
    # terminal. Point them at the one command instead of deciding for them.
    _ = service  # imported for symmetry; install is invoked by the user.
    return 0


def _service_loaded() -> bool:
    """Best-effort check of whether the LaunchAgent is loaded."""
    try:
        from whiz.dictate import service

        res = service._run(["launchctl", "list", service.LABEL])
        return res.returncode == 0
    except Exception:  # noqa: BLE001
        return False