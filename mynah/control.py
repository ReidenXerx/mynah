"""The control socket: how anything outside the process drives dictation.

On macOS the app owns its own hotkey (pynput) and draws its own indicator
(AppKit), so nothing else needs to talk to it. On Wayland neither is true: a
client cannot grab a global hotkey, and the desktop shell — not us — draws the
UI. The compositor binds the key and runs ``mynah toggle``; the shell plugin
runs ``mynah watch`` and draws what it hears.

So this is a small line-delimited JSON server on a unix socket:

    {"cmd": "toggle"}      -> {"ok": true, "state": "listening"}
    {"cmd": "start"}       -> {"ok": true, "state": "listening"}
    {"cmd": "stop"}        -> {"ok": true, "state": "idle"}
    {"cmd": "status"}      -> {"ok": true, "state": "idle", "pid": 4242}
    {"cmd": "quit"}        -> {"ok": true}
    {"cmd": "subscribe"}   -> {"ok": true, "state": "idle"} then one line per
                              event until the client goes away:
                              {"event": "state", "state": "listening"}
                              {"event": "level", "level": 0.42}
                              {"event": "text",  "text": "the fee is computed"}

Anything that can write to this socket can make the machine dictate, and
anything that can read it hears every word typed. So the socket lives in a
0700 directory under ``$XDG_RUNTIME_DIR`` (tmpfs, per-user, wiped at logout)
with mode 0600, and the server refuses to start if that directory is not ours
or is group/world-accessible — an attacker who wins the race for the path gets
a socket nobody connects to, rather than one everyone does.

Events are published through the module-level :func:`publish`, which is inert
when no server is running. That is deliberate: the Linux indicator and injector
call it unconditionally, so neither has to know whether anyone is listening.
"""

from __future__ import annotations

import json
import logging
import os
import socket
import stat
import threading
import time
from pathlib import Path
from typing import Any, Callable

logger = logging.getLogger("mynah.control")

# How long a slow subscriber may block a publish before we give up on it. Level
# events arrive ~33x a second; a shell that stops reading must never be able to
# stall the audio thread.
_SEND_TIMEOUT = 0.25

# Level events are the firehose. Coalesce them: a UI cannot show more than the
# compositor's frame rate anyway, and the socket buffer should not fill with
# stale amplitudes while a subscriber is busy.
_LEVEL_MIN_INTERVAL = 1 / 30


def socket_path() -> Path:
    """Where the control socket lives.

    ``$XDG_RUNTIME_DIR`` is the right home for it: tmpfs, mode 0700, owned by
    the user, and cleared when the last session ends. Without it (a bare ssh
    session, a cron job) fall back to ``/tmp/mynah-<uid>``, which we create
    ourselves with the same permissions.
    """
    override = os.environ.get("MYNAH_SOCKET")
    if override:
        return Path(override)
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    base = Path(runtime) / "mynah" if runtime else Path(f"/tmp/mynah-{os.getuid()}")
    return base / "control.sock"


def _prepare_dir(path: Path) -> None:
    """Create the socket's directory, and refuse to use one we do not own.

    A directory someone else can write to is a directory where someone else can
    replace the socket with their own, and then every ``mynah watch`` in the
    session is talking to them.
    """
    directory = path.parent
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = directory.stat()
    if info.st_uid != os.getuid():
        raise PermissionError(f"{directory} is not owned by this user")
    if info.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise PermissionError(f"{directory} is accessible to other users")


class ControlServer:
    """Serves the control socket for one running engine."""

    def __init__(
        self,
        *,
        on_toggle: Callable[[], None],
        on_start: Callable[[], None],
        on_stop: Callable[[], None],
        on_quit: Callable[[], None],
        state: Callable[[], str],
        path: Path | None = None,
    ) -> None:
        self.path = path or socket_path()
        self._handlers: dict[str, Callable[[], None]] = {
            "toggle": on_toggle,
            "start": on_start,
            "stop": on_stop,
            "quit": on_quit,
        }
        self._state = state
        self._server: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._subscribers: list[socket.socket] = []
        self._lock = threading.Lock()

    # ---------- lifecycle ----------

    def start(self) -> None:
        """Bind the socket and serve in a background thread."""
        _prepare_dir(self.path)
        # A stale socket from a killed process would make bind() fail with
        # EADDRINUSE. Connect first: if someone answers, another mynah owns
        # this socket and we must not steal it.
        if self.path.exists():
            if self._someone_home():
                raise RuntimeError(f"another mynah is already running on {self.path}")
            self.path.unlink()
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(self.path))
        os.chmod(self.path, 0o600)
        server.listen(8)
        server.settimeout(0.5)
        self._server = server
        self._thread = threading.Thread(target=self._accept_loop, name="mynah-control", daemon=True)
        self._thread.start()
        logger.debug("control socket listening on %s", self.path)
        _set_active(self)

    def _someone_home(self) -> bool:
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        probe.settimeout(0.2)
        try:
            probe.connect(str(self.path))
            return True
        except OSError:
            return False
        finally:
            probe.close()

    def stop(self) -> None:
        """Stop serving and remove the socket."""
        _set_active(None)
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
            self._thread = None
        if self._server:
            self._server.close()
            self._server = None
        with self._lock:
            for sub in self._subscribers:
                sub.close()
            self._subscribers.clear()
        try:
            self.path.unlink()
        except OSError:
            pass

    # ---------- serving ----------

    def _accept_loop(self) -> None:
        assert self._server is not None
        while not self._stop.is_set():
            try:
                conn, _ = self._server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn: socket.socket) -> None:
        subscribed = False
        try:
            conn.settimeout(None)
            with conn.makefile("r", encoding="utf-8") as reader:
                for line in reader:
                    line = line.strip()
                    if not line:
                        continue
                    reply, subscribe = self._handle(line)
                    self._send(conn, reply)
                    if subscribe:
                        subscribed = True
                        with self._lock:
                            self._subscribers.append(conn)
                        # The subscriber's connection now belongs to the
                        # publish path; stop reading from this thread.
                        return
        except OSError:
            pass
        finally:
            if not subscribed:
                conn.close()

    def _handle(self, line: str) -> tuple[dict[str, Any], bool]:
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            # Be forgiving: a bare word is a command too, so `echo toggle | nc`
            # works from a script without quoting JSON.
            message = {"cmd": line}
        if not isinstance(message, dict):
            return {"ok": False, "error": "expected an object"}, False
        cmd = str(message.get("cmd", "")).strip().lower()
        if cmd == "subscribe":
            return {"ok": True, "state": self._state()}, True
        if cmd == "status":
            return {"ok": True, "state": self._state(), "pid": os.getpid()}, False
        handler = self._handlers.get(cmd)
        if handler is None:
            return {"ok": False, "error": f"unknown command {cmd!r}"}, False
        # Ending a session waits for the transcription queue to drain, which is
        # seconds of whisper. Running that here would hold the reply for the
        # whole time, and the key that stopped dictation would look wedged. So
        # the command is acknowledged now and carried out on its own thread;
        # what actually happened arrives as state events, which is where a
        # caller should be reading the truth from anyway.
        threading.Thread(
            target=self._carry_out, args=(cmd, handler), name=f"mynah-{cmd}", daemon=True
        ).start()
        return {"ok": True, "state": self._state()}, False

    def _carry_out(self, cmd: str, handler: Callable[[], None]) -> None:
        try:
            handler()
        except Exception as e:  # noqa: BLE001
            logger.warning("control command %s failed: %s", cmd, e)
            self.publish({"event": "error", "command": cmd, "error": str(e)})

    @staticmethod
    def _send(conn: socket.socket, payload: dict[str, Any]) -> None:
        conn.sendall((json.dumps(payload) + "\n").encode("utf-8"))

    # ---------- publishing ----------

    def publish(self, event: dict[str, Any]) -> None:
        """Send one event to every subscriber, dropping those that have gone."""
        with self._lock:
            if not self._subscribers:
                return
            subscribers = list(self._subscribers)
        blob = (json.dumps(event) + "\n").encode("utf-8")
        dead = []
        for sub in subscribers:
            try:
                sub.settimeout(_SEND_TIMEOUT)
                sub.sendall(blob)
            except OSError:
                dead.append(sub)
        if dead:
            with self._lock:
                for sub in dead:
                    if sub in self._subscribers:
                        self._subscribers.remove(sub)
                    sub.close()


# ---------- module-level access ----------
#
# The indicator and the injector publish without knowing whether a server
# exists, so the active server (there is at most one per process) lives here.

_active: ControlServer | None = None
_active_lock = threading.Lock()
_last_level_at = 0.0


def _set_active(server: ControlServer | None) -> None:
    global _active
    with _active_lock:
        _active = server


def active() -> ControlServer | None:
    with _active_lock:
        return _active


def publish(event: dict[str, Any]) -> None:
    """Publish an event to subscribers; a no-op when nothing is serving."""
    server = active()
    if server is None:
        return
    server.publish(event)


def publish_level(level: float) -> None:
    """Publish a mic level, coalesced to at most ~30 a second.

    Called from the capture thread for every 30 ms frame. The rate limit is
    here rather than in the caller so every future publisher gets it.
    """
    global _last_level_at
    server = active()
    if server is None:
        return
    now = time.monotonic()
    if now - _last_level_at < _LEVEL_MIN_INTERVAL:
        return
    _last_level_at = now
    server.publish({"event": "level", "level": round(float(level), 4)})


# ---------- client ----------


class ControlError(RuntimeError):
    """Raised when no running mynah answers on the control socket."""


def connect(path: Path | None = None, timeout: float = 2.0) -> socket.socket:
    """Connect to a running mynah, or raise :class:`ControlError`."""
    target = path or socket_path()
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    conn.settimeout(timeout)
    try:
        conn.connect(str(target))
    except OSError as e:
        conn.close()
        raise ControlError(
            f"no running mynah on {target} ({e.strerror or e}).\n"
            "Start one with:  mynah        (or: systemctl --user start mynah)"
        ) from None
    return conn


def command(cmd: str, path: Path | None = None, timeout: float = 5.0) -> dict[str, Any]:
    """Send one command to a running mynah and return its reply."""
    conn = connect(path, timeout=timeout)
    try:
        conn.sendall((json.dumps({"cmd": cmd}) + "\n").encode("utf-8"))
        with conn.makefile("r", encoding="utf-8") as reader:
            line = reader.readline()
    except TimeoutError:
        raise ControlError(
            f"mynah did not answer {cmd!r} within {timeout:g}s. It may be busy "
            "starting up; try again, or check: journalctl --user -u mynah -n 20"
        ) from None
    except OSError as e:
        raise ControlError(f"lost the connection to mynah ({e})") from None
    finally:
        conn.close()
    if not line:
        raise ControlError("mynah closed the connection without replying")
    try:
        return json.loads(line)
    except json.JSONDecodeError:
        raise ControlError("mynah answered with something that was not JSON") from None
