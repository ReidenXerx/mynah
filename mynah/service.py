"""launchd LaunchAgent management for ``mynah``.

Manages a per-user LaunchAgent that starts ``mynah`` at login and
keeps it running (``KeepAlive``). This is the \"always-on\" service: the
dictation engine runs in the background, the idle indicator stays visible,
and the hotkey is armed without keeping a terminal open.

Stdlib-only (no new dependency): the plist is emitted by hand and the
``launchctl`` tool is invoked via ``subprocess``.

Layout:
- ``~/Library/LaunchAgents/com.reidenxerx.mynah.plist``
- ``~/Library/Logs/mynah.log`` (combined stdout/stderr)

The agent runs as a separate process from any terminal ``mynah``, so it
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

LABEL = "com.reidenxerx.mynah"

_LAUNCH_AGENTS_DIR = Path.home() / "Library" / "LaunchAgents"
_LOG_PATH = Path.home() / "Library" / "Logs" / "mynah.log"
# A renamed copy of the framework Python binary, placed at a stable path
# outside the pipx venv so it survives ``pipx install --force`` during
# ``pipx upgrade mynah``. The kernel process name (``p_comm``) is set from the
# binary basename at ``execve``, so a binary named ``mynah`` shows as
# "mynah" in Activity Monitor / Force Quit — not "Python". Because the path
# is stable, the Accessibility/TCC permission granted to it persists
# across upgrades (the pipx venv rebuild doesn't touch it).
_RUNNER_DIR = Path.home() / ".local" / "share" / "mynah"
_RUNNER_NAME = "mynah"


def plist_path() -> Path:
    """Return the on-disk path of the LaunchAgent plist."""
    return _LAUNCH_AGENTS_DIR / f"{LABEL}.plist"


def _venv_bin_dir() -> Path | None:
    """Return the pipx venv ``bin`` dir that owns the ``mynah`` script, or None.

    ``shutil.which("mynah")`` resolves to ``~/.local/bin/mynah`` (a pipx app
    symlink). We resolve through it to the real script inside the venv and
    return its parent directory.
    """
    which = shutil.which("mynah")
    if not which:
        return None
    real = Path(which).resolve()
    if real.name == "mynah":
        return real.parent
    return None


def _framework_python_binary() -> Path | None:
    """Return the real framework Python binary (the one inside Python.app).

    The venv ``bin/python`` is a symlink to ``bin/python3.x``, which is a
    *stub* that re-execs from ``Python.app/Contents/MacOS/Python``. A copy
    of the stub inherits that behaviour, so the kernel process name still
    resolves to "Python". We must copy the actual ``Python.app`` binary
    instead — a copy of that, named ``mynah``, reports ``comm=mynah``.
    """
    venv_bin = _venv_bin_dir()
    if venv_bin is None:
        return None
    venv_python = venv_bin / "python"
    if not venv_python.exists():
        return None
    real = venv_python.resolve()
    # The symlink resolves to .../bin/python3.x (the stub). The real binary
    # is the sibling Python.app bundle's MacOS/Python.
    fw_root = real.parent.parent  # .../Python.framework/Versions/3.x
    app_python = fw_root / "Resources" / "Python.app" / "Contents" / "MacOS" / "Python"
    if app_python.exists():
        return app_python
    # Fallback: some builds don't ship Python.app — use the resolved binary.
    return real if real.exists() else None


def _venv_site_packages() -> str | None:
    """Return the venv's ``site-packages`` path for PYTHONPATH."""
    venv_bin = _venv_bin_dir()
    if venv_bin is None:
        return None
    # Derive without importing sysconfig from the venv: the layout is
    # <venv>/lib/python3.x/site-packages. Scan for it so we don't hardcode
    # the minor version.
    lib = venv_bin.parent / "lib"
    for pydir in sorted(lib.iterdir(), reverse=True):
        sp = pydir / "site-packages"
        if sp.is_dir():
            return str(sp)
    return None


def _ensure_runner() -> str | None:
    """Create a renamed copy of the framework Python binary named ``mynah``.

    Activity Monitor and Force Quit display the *kernel process name*
    (``proc_name``/``p_comm``), set at ``execve`` from the actual binary's
    basename — NOT from ``argv[0]`` (which ``setproctitle`` changes). The
    pipx ``mynah`` console script is a Python text file with a ``#!/.../python``
    shebang, so the real executable is always
    ``.../Python.app/Contents/MacOS/Python`` and the system UI shows
    "Python".

    A *copy* of the framework ``Python.app`` binary named ``mynah`` placed
    at a stable path (``~/.local/share/mynah/mynah``) reports ``comm=mynah`` —
    so Activity Monitor shows "mynah". Since it's outside the venv, the
    venv's ``site-packages`` is injected via ``PYTHONPATH`` in the plist.
    The stable path means the Accessibility/TCC permission granted to it
    persists across ``pipx install --force`` during an upgrade.

    Returns the absolute path to the runner binary, or None if it can't
    be built (the caller falls back to the plain ``mynah`` script).
    """
    src = _framework_python_binary()
    if src is None:
        return None

    site = _venv_site_packages()
    if site is None:
        return None

    _RUNNER_DIR.mkdir(parents=True, exist_ok=True)
    runner = _RUNNER_DIR / _RUNNER_NAME

    # Only copy if missing or the source changed (avoid rewriting on every
    # call — rewriting would invalidate the TCC Accessibility grant).
    need_copy = (
        not runner.exists()
        or runner.stat().st_size != src.stat().st_size
        or runner.stat().st_mtime < src.stat().st_mtime
    )
    if need_copy:
        try:
            shutil.copy2(src, runner)
            os.chmod(runner, 0o755)
        except OSError:
            return None

    # Sanity check: the runner must start and find mynah via PYTHONPATH —
    # and not just the top-level package: `mynah` immediately
    # imports mynah.engine, so an install where that module (or
    # its imports) breaks must fall back to the plain script HERE (L5,
    # wave-2) instead of yielding a LaunchAgent that crash-loops on
    # launch. The heavy deps (mlx_whisper, sounddevice, pynput) stay lazy
    # inside functions, so this probe stays cheap.
    env = os.environ.copy()
    env["PYTHONPATH"] = site
    probe = subprocess.run(
        [str(runner), "-c", "import mynah.engine"],
        capture_output=True, text=True, check=False, timeout=15,
        env=env,
    )
    if probe.returncode != 0:
        return None
    return str(runner)


def _resolve_mynah_bin() -> tuple[list[str], dict[str, str]]:
    """Resolve the command (and env vars) to launch ``mynah``.

    Prefer a renamed Python runner binary (``mynah``) so the process shows
    as ``mynah`` (not ``Python``) in Activity Monitor / Force Quit. The
    runner needs ``PYTHONPATH`` pointed at the venv's ``site-packages``.
    Fall back to the ``mynah`` console script on PATH (what pipx installs
    into ``~/.local/bin``); fall back further to ``python -m mynah`` using
    the current interpreter so the agent still works when mynah is installed
    editable or run from a venv without the console script.

    Env vars emitted on EVERY argv path (M14, wave-2):

    - ``MYNAH_SERVICE=1`` — marks the process as running under
      launchd. engine.py's ``_run_with_appkit`` reads it to decide
      between returning 0 (which KeepAlive silently turns into a
      relaunch loop) and returning 1 on a menu-bar setup failure, so
      the agent must actually receive it whichever argv path resolved.
    - ``MYNAH_CONFIG_DIR`` — passed through when set in the installer's
      environment: mynah.config reads it at import, so a custom config
      dir used for the CLI must survive into the agent too.

    Returns ``(argv, env_vars)`` where ``env_vars`` is a dict to emit as
    the plist's ``EnvironmentVariables`` key (empty dict when no override
    is needed).
    """
    env: dict[str, str] = {"MYNAH_SERVICE": "1"}
    config_dir = os.environ.get("MYNAH_CONFIG_DIR")
    if config_dir:
        env["MYNAH_CONFIG_DIR"] = config_dir
    runner = _ensure_runner()
    if runner:
        site = _venv_site_packages() or ""
        env["PYTHONPATH"] = site
        return [runner, "-m", "mynah"], env
    which = shutil.which("mynah")
    if which:
        return [which], env
    return [sys.executable, "-m", "mynah"], env


def build_plist() -> str:
    """Build the LaunchAgent plist XML (string)."""
    argv, env_vars = _resolve_mynah_bin()
    # Bare `mynah` IS the dictation command (cli.cmd_run is the parser's
    # default), so nothing is appended. It used to append `dictate`, from
    # when this was `whiz dictate`; under launchd that argv now exits 2 on
    # every launch and KeepAlive turns it into a 30-second crash loop.

    args_xml = "\n".join(
        f"    <string>{escape(a)}</string>" for a in argv
    )
    log = str(_LOG_PATH)
    env_xml = ""
    if env_vars:
        env_items = "\n".join(
            f"      <key>{escape(k)}</key>\n      <string>{escape(v)}</string>"
            for k, v in env_vars.items()
        )
        env_xml = (
            f"  <key>EnvironmentVariables</key>\n"
            f"  <dict>\n"
            f"{env_items}\n"
            f"  </dict>\n"
        )
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
        f"{env_xml}"
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

    print(f"Installed mynah service (LaunchAgent {LABEL}).")
    print(f"  plist: {plist}")
    print(f"  log:   {_LOG_PATH}")
    print()
    print("The service starts at login and stays running (KeepAlive).")
    print("Manage with: mynah service status | uninstall")
    print()
    print(
        "NOTE: the background agent is a separate process from terminal mynah "
        "and needs its own Accessibility permission. Grant it in System "
        "Settings → Privacy & Security → Accessibility, then restart the "
        "agent: mynah service uninstall && mynah service install"
    )
    return 0


def uninstall() -> int:
    """Unload and remove the LaunchAgent. Returns 0 even if not installed."""
    plist = plist_path()
    if plist.exists():
        _run(["launchctl", "unload", str(plist)])
        plist.unlink(missing_ok=True)
        print(f"Uninstalled mynah service (removed {plist}).")
    else:
        # Best-effort unload in case the file was removed out of band.
        _run(["launchctl", "remove", LABEL])
        print("mynah service was not installed (no plist found).")
    return 0


def status() -> int:
    """Print the service status: loaded/not-loaded, PID, last exit."""
    res = _run(["launchctl", "list", LABEL])
    if res.returncode != 0:
        print(f"mynah service: not loaded ({LABEL}).")
        print(f"  plist present: {plist_path().exists()}")
        print("Install with: mynah service install")
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
    print(f"mynah service: loaded ({LABEL})")
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