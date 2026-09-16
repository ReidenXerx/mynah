"""The login service on Linux: a ``systemd --user`` unit.

The Linux counterpart of ``service.py``'s LaunchAgent. Same promise — mynah is
running before you think to want it, the key is armed, and nothing keeps a
terminal open — expressed in the other init system's terms.

Two details are not decoration:

``PartOf`` / ``WantedBy=graphical-session.target`` ties the daemon to the
graphical session rather than to login. mynah types through the Wayland
compositor; started before there is one, it can only fail, and left running
after the session ends it holds a microphone for nobody.

``StartLimitBurst`` gives up after three failures in a minute. A service that
cannot start is a service that needs a person, not a retry: the macOS side
learned this from a ``KeepAlive`` loop that relaunched a broken argv every 30
seconds and reported itself as installed the whole time.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

UNIT_NAME = "mynah.service"

_UNIT_DIR = Path(
    os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")
) / "systemd" / "user"


def unit_path() -> Path:
    return _UNIT_DIR / UNIT_NAME


def _exec_start() -> str:
    """The ExecStart line. systemd needs an absolute path, not a name on PATH."""
    found = shutil.which("mynah")
    if found:
        return found
    # Editable installs and venvs without the console script still work.
    return f"{sys.executable} -m mynah"


def build_unit() -> str:
    """The unit file, as text."""
    environment = ["Environment=MYNAH_SERVICE=1"]
    config_dir = os.environ.get("MYNAH_CONFIG_DIR")
    if config_dir:
        # mynah.config reads this at import, so a custom config dir used for
        # the CLI has to survive into the service or they are two mynahs.
        environment.append(f"Environment=MYNAH_CONFIG_DIR={config_dir}")
    return f"""[Unit]
Description=Mynah dictation
Documentation=https://duduphudu.app/mynah/
PartOf=graphical-session.target
After=graphical-session.target
StartLimitIntervalSec=60
StartLimitBurst=3

[Service]
Type=simple
ExecStart={_exec_start()}
{chr(10).join(environment)}
Restart=on-failure
RestartSec=5
Slice=session.slice

[Install]
WantedBy=graphical-session.target
"""


def _systemctl(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["systemctl", "--user", *args],
        capture_output=True,
        text=True,
        check=False,
    )


def install() -> int:
    """Write the unit, enable it, and start it now."""
    if shutil.which("systemctl") is None:
        print(
            "systemd is not available here, so there is no login service to install.\n"
            "Start mynah from your compositor's autostart instead:  mynah",
            file=sys.stderr,
        )
        return 1
    path = unit_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(build_unit(), encoding="utf-8")
    print(f"Wrote {path}", file=sys.stderr)

    reload_result = _systemctl("daemon-reload")
    if reload_result.returncode != 0:
        print(f"systemctl daemon-reload failed: {reload_result.stderr.strip()}", file=sys.stderr)
        return 1
    enable = _systemctl("enable", "--now", UNIT_NAME)
    if enable.returncode != 0:
        print(f"Could not enable {UNIT_NAME}: {enable.stderr.strip()}", file=sys.stderr)
        return 1
    print(
        f"{UNIT_NAME} is enabled and running.\n"
        "  Bind a key in your compositor to:  mynah toggle\n"
        f"  Logs:    journalctl --user -u {UNIT_NAME} -f\n"
        f"  Status:  mynah service status",
        file=sys.stderr,
    )
    return 0


def uninstall() -> int:
    """Stop the service, disable it, and remove the unit."""
    path = unit_path()
    if shutil.which("systemctl") is not None:
        _systemctl("disable", "--now", UNIT_NAME)
    existed = path.exists()
    if existed:
        path.unlink()
    if shutil.which("systemctl") is not None:
        _systemctl("daemon-reload")
    print(
        f"Removed {path}" if existed else f"No unit at {path} — nothing to remove.",
        file=sys.stderr,
    )
    return 0


def status() -> int:
    """Report whether the service is installed, enabled and running."""
    path = unit_path()
    if not path.exists():
        print(
            f"Not installed (no {path}).\n"
            "  Install it with:  mynah service install",
            file=sys.stderr,
        )
        return 1
    if shutil.which("systemctl") is None:
        print(f"Unit file at {path}, but systemctl is not available.", file=sys.stderr)
        return 1
    enabled = _systemctl("is-enabled", UNIT_NAME).stdout.strip() or "unknown"
    active = _systemctl("is-active", UNIT_NAME).stdout.strip() or "unknown"
    print(f"{UNIT_NAME}: {active}, {enabled}\n  {path}", file=sys.stderr)
    if active != "active":
        print(
            f"  Why:  journalctl --user -u {UNIT_NAME} -n 20 --no-pager",
            file=sys.stderr,
        )
        return 1
    return 0
