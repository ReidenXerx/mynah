"""launchd LaunchAgent management for ``whiz dictate``.

Manages a per-user LaunchAgent that starts ``whiz dictate`` at login and
keeps it running (``KeepAlive``). This is the \"always-on\" service: the
dictation engine runs in the background, the idle indicator stays visible,
and the hotkey is armed without keeping a terminal open.

Stdlib-only (no new dependency): the plist is emitted by hand and the
``launchctl`` tool is invoked via ``subprocess``.

Layout:
- ``~/Library/LaunchAgents/com.reidenxerx.whiz.dictate.plist``
- ``~/Library/Logs/whiz-dictate.log`` (combined stdout/stderr)

The agent runs as a separate process from any terminal ``whiz``, so it
needs its own Accessibility grant in System Settings → Privacy & Security
(pynput global hotkey + CGEvent text injection both require it).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from xml.sax.saxutils import escape

LABEL = "com.reidenxerx.whiz.dictate"

_LAUNCH_AGENTS_DIR = Path.home() / "Library" / "LaunchAgents"
_LOG_PATH = Path.home() / "Library" / "Logs" / "whiz-dictate.log"
# A renamed copy of the venv Python binary, placed in the venv ``bin`` dir so
# it finds ``pyvenv.cfg`` and the venv ``site-packages``. The kernel process
# name (``p_comm``) is set from the binary basename at ``execve``, so a binary
# named ``whiz-runner`` shows as "whiz-runner" in Activity Monitor / Force
# Quit — not "Python". Recreated on every ``service.install()`` (which runs
# after ``pipx install --force`` during ``whiz upgrade``), so it survives
# venv rebuilds.
_RUNNER_NAME = "whiz-runner"


def plist_path() -> Path:
    """Return the on-disk path of the LaunchAgent plist."""
    return _LAUNCH_AGENTS_DIR / f"{LABEL}.plist"


def log_path() -> Path:
    """Return the combined stdout/stderr log path for the agent."""
    return _LOG_PATH


def _venv_bin_dir() -> Path | None:
    """Return the pipx venv ``bin`` dir that owns the ``whiz`` script, or None.

    ``shutil.which("whiz")`` resolves to ``~/.local/bin/whiz`` (a pipx app
    symlink). We resolve through it to the real script inside the venv and
    return its parent directory.
    """
    which = shutil.which("whiz")
    if not which:
        return None
    real = Path(which).resolve()
    if real.name == "whiz":
        return real.parent
    return None


def _ensure_runner() -> str | None:
    """Create a renamed copy of the venv Python binary named ``whiz-runner``.

    Activity Monitor and Force Quit display the *kernel process name*
    (``proc_name``/``p_comm``), set at ``execve`` from the actual binary's
    basename — NOT from ``argv[0]`` (which ``setproctitle`` changes). The
    pipx ``whiz`` console script is a Python text file with a ``#!/.../python``
    shebang, so the real executable is always
    ``.../Python.app/Contents/MacOS/Python`` and the system UI shows
    "Python".

    A *copy* of the framework Python binary named ``whiz-runner`` placed in
    the venv ``bin`` dir is still recognised as a venv interpreter (it finds
    ``pyvenv.cfg`` in its parent), finds the venv ``site-packages``, and
    reports ``comm=whiz-runner`` — so Activity Monitor shows "whiz-runner".

    Returns the absolute path to the runner binary, or None if it can't be
    built (the caller falls back to the plain ``whiz`` script).
    """
    venv_bin = _venv_bin_dir()
    if venv_bin is None:
        return None

    venv_python = venv_bin / "python"
    if not venv_python.exists():
        return None
    # ``venv_python`` is a symlink to the framework Python binary.
    real_python = venv_python.resolve()
    if not real_python.exists():
        return None

    runner = venv_bin / _RUNNER_NAME

    # Only copy if missing or the source changed (avoid rewriting on every call).
    need_copy = (
        not runner.exists()
        or runner.stat().st_size != real_python.stat().st_size
        or runner.stat().st_mtime < real_python.stat().st_mtime
    )
    if need_copy:
        try:
            shutil.copy2(real_python, runner)
            os.chmod(runner, 0o755)
        except OSError:
            return None

    # Sanity check: the runner must start and find the venv. If it can't
    # import whiz, don't use it — fall back to the plain whiz script.
    probe = subprocess.run(
        [str(runner), "-c", "import whiz"],
        capture_output=True, text=True, check=False, timeout=15,
    )
    if probe.returncode != 0:
        return None
    return str(runner)


def _resolve_whiz_bin() -> list[str]:
    """Resolve the command to launch ``whiz dictate``.

    Prefer a renamed Python runner binary (``whiz-runner``) so the process
    shows as ``whiz-runner`` (not ``Python``) in Activity Monitor / Force
    Quit. Fall back to the ``whiz`` console script on PATH (what pipx
    installs into ``~/.local/bin``); fall back further to ``python -m whiz``
    using the current interpreter so the agent still works when whiz is
    installed editable or run from a venv without the console script.
    """
    runner = _ensure_runner()
    if runner:
        return [runner, "-m", "whiz"]
    which = shutil.which("whiz")
    if which:
        return [which]
    return [sys.executable, "-m", "whiz"]


def build_plist() -> str:
    """Build the LaunchAgent plist XML (string)."""
    argv = _resolve_whiz_bin()
    # Ensure the dictation command is explicit (not just bare `whiz`).
    if argv[-1] != "dictate" and "dictate" not in argv:
        argv = argv + ["dictate"]

    args_xml = "\n".join(
        f"    <string>{escape(a)}</string>" for a in argv
    )
    log = str(_LOG_PATH)
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0">\n'
        "<dict>\n"
        f"  <key>Label</key>\n"
        f"  <string>{escape(LABEL)}</string>\n"
        f"  <key>ProgramArguments</key>\n"
        f"  <array>\n"
        f"{args_xml}\n"
        f"  </array>\n"
        f"  <key>RunAtLoad</key>\n"
        f"  <true/>\n"
        f"  <key>KeepAlive</key>\n"
        f"  <true/>\n"
        f"  <key>ThrottleInterval</key>\n"
        f"  <integer>30</integer>\n"
        f"  <key>ProcessType</key>\n"
        f"  <string>Interactive</string>\n"
        f"  <key>StandardOutPath</key>\n"
        f"  <string>{escape(log)}</string>\n"
        f"  <key>StandardErrorPath</key>\n"
        f"  <string>{escape(log)}</string>\n"
        f"</dict>\n"
        f"</plist>\n"
    )


def _run(cmd: list[str]) -> subprocess.CompletedProcess:
    """Run a command, capturing output. Never raises on non-zero rc."""
    return subprocess.run(cmd, capture_output=True, text=True, check=False)


def install() -> int:
    """Install and load the LaunchAgent so dictation starts at login.

    Refreshes if already loaded. Returns 0 on success, 1 if launchctl fails.
    """
    _LAUNCH_AGENTS_DIR.mkdir(parents=True, exist_ok=True)
    _LOG_PATH.parent.mkdir(parents=True, exist_ok=True)

    plist = plist_path()
    # If already loaded, unload first so the new plist takes effect.
    if plist.exists():
        _run(["launchctl", "unload", str(plist)])

    plist.write_text(build_plist(), encoding="utf-8")

    res = _run(["launchctl", "load", str(plist)])
    if res.returncode != 0:
        print(
            f"Failed to load LaunchAgent ({res.returncode}):\n"
            f"{res.stderr.strip() or res.stdout.strip()}\n"
            f"Plist written to {plist}; run `launchctl load {plist}` manually.",
            file=sys.stderr,
        )
        return 1

    print(f"Installed whiz dictate service (LaunchAgent {LABEL}).")
    print(f"  plist: {plist}")
    print(f"  log:   {_LOG_PATH}")
    print()
    print("The service starts at login and stays running (KeepAlive).")
    print("Manage with: whiz dictate service status | uninstall")
    print()
    print(
        "NOTE: the background agent is a separate process from terminal whiz "
        "and needs its own Accessibility permission. Grant it in System "
        "Settings → Privacy & Security → Accessibility, then restart the "
        "agent: whiz dictate service uninstall && whiz dictate service install"
    )
    return 0


def uninstall() -> int:
    """Unload and remove the LaunchAgent. Returns 0 even if not installed."""
    plist = plist_path()
    if plist.exists():
        _run(["launchctl", "unload", str(plist)])
        plist.unlink(missing_ok=True)
        print(f"Uninstalled whiz dictate service (removed {plist}).")
    else:
        # Best-effort unload in case the file was removed out of band.
        _run(["launchctl", "remove", LABEL])
        print("whiz dictate service was not installed (no plist found).")
    return 0


def status() -> int:
    """Print the service status: loaded/not-loaded, PID, last exit."""
    res = _run(["launchctl", "list", LABEL])
    if res.returncode != 0:
        print(f"whiz dictate service: not loaded ({LABEL}).")
        print(f"  plist present: {plist_path().exists()}")
        print("Install with: whiz dictate service install")
        return 0

    # `launchctl list <label>` output format (modern launchctl):
    #   {
    #     "Label" = "...";
    #     "PID" = 1234;
    #     "LastExitStatus" = 0;
    #     ...
    #   }
    out = res.stdout
    pid = _extract_field(out, "PID") or "-"
    last_exit = _extract_field(out, "LastExitStatus") or "-"
    print(f"whiz dictate service: loaded ({LABEL})")
    print(f"  PID:            {pid}")
    print(f"  LastExitStatus: {last_exit}")
    print(f"  plist:          {plist_path()}")
    print(f"  log:            {_LOG_PATH}")
    return 0


def _extract_field(plist_output: str, key: str) -> str | None:
    """Extract a value for ``key`` from ``launchctl list`` dict-style output."""
    import re

    m = re.search(rf'"{re.escape(key)}"\s*=\s*([^;]+);', plist_output)
    if m:
        return m.group(1).strip().strip('"')
    return None