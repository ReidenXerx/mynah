"""The Linux side: the control socket, the providers, the systemd unit.

These run on any platform — nothing here needs Wayland, whisper.cpp or wtype to
be installed, because everything that touches them is resolved through a
function the test can point somewhere else.
"""

from __future__ import annotations

import json
import os
import socket
import stat
import sys
import threading
import time
from pathlib import Path
from unittest import mock

import pytest

from mynah import control
from mynah.providers import linux_inject, linux_stt


# ---------------------------------------------------------------------------
# Control socket
# ---------------------------------------------------------------------------


class FakeEngine:
    """Records what the socket asked the engine to do."""

    def __init__(self) -> None:
        self.calls: list[str] = []
        self.state = "idle"

    def toggle(self) -> None:
        self.calls.append("toggle")
        self.state = "listening" if self.state == "idle" else "idle"

    def start(self) -> None:
        self.calls.append("start")
        self.state = "listening"

    def stop(self) -> None:
        self.calls.append("stop")
        self.state = "idle"

    def quit(self) -> None:
        self.calls.append("quit")


@pytest.fixture
def server(tmp_path):
    engine = FakeEngine()
    srv = control.ControlServer(
        on_toggle=engine.toggle,
        on_start=engine.start,
        on_stop=engine.stop,
        on_quit=engine.quit,
        state=lambda: engine.state,
        path=tmp_path / "sock" / "control.sock",
    )
    srv.start()
    try:
        yield srv, engine
    finally:
        srv.stop()


def _talk(path: Path, *messages: str, expect: int = 1) -> list[dict]:
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    conn.settimeout(3)
    conn.connect(str(path))
    try:
        for message in messages:
            conn.sendall((message + "\n").encode())
        replies = []
        with conn.makefile("r", encoding="utf-8") as reader:
            for _ in range(expect):
                line = reader.readline()
                if not line:
                    break
                replies.append(json.loads(line))
        return replies
    finally:
        conn.close()


def test_commands_reach_the_engine(server):
    srv, engine = server
    replies = _talk(srv.path, '{"cmd": "toggle"}')
    assert replies[0] == {"ok": True, "state": "listening"}
    assert engine.calls == ["toggle"]


def test_a_bare_word_is_a_command_too(server):
    """`echo toggle | socat - UNIX:...` should work without quoting JSON."""
    srv, engine = server
    replies = _talk(srv.path, "stop")
    assert replies[0]["ok"] is True
    assert engine.calls == ["stop"]


def test_status_reports_state_and_pid(server):
    srv, _engine = server
    reply = _talk(srv.path, '{"cmd": "status"}')[0]
    assert reply["state"] == "idle"
    assert reply["pid"] == os.getpid()


def test_unknown_command_is_refused_not_ignored(server):
    srv, engine = server
    reply = _talk(srv.path, '{"cmd": "selfdestruct"}')[0]
    assert reply["ok"] is False
    assert "selfdestruct" in reply["error"]
    assert engine.calls == []


def test_a_failing_handler_answers_instead_of_hanging(tmp_path):
    def explode() -> None:
        raise RuntimeError("no microphone")

    srv = control.ControlServer(
        on_toggle=explode, on_start=explode, on_stop=explode, on_quit=explode,
        state=lambda: "idle", path=tmp_path / "s" / "control.sock",
    )
    srv.start()
    try:
        reply = _talk(srv.path, "toggle")[0]
    finally:
        srv.stop()
    assert reply == {"ok": False, "error": "no microphone"}


def test_subscribers_receive_events(server):
    srv, _engine = server
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    conn.settimeout(3)
    conn.connect(str(srv.path))
    try:
        conn.sendall(b'{"cmd": "subscribe"}\n')
        reader = conn.makefile("r", encoding="utf-8")
        assert json.loads(reader.readline())["ok"] is True
        # Give the server a moment to move this connection to the publish list.
        deadline = time.monotonic() + 2
        while not srv._subscribers and time.monotonic() < deadline:
            time.sleep(0.01)
        srv.publish({"event": "state", "state": "listening"})
        assert json.loads(reader.readline()) == {"event": "state", "state": "listening"}
    finally:
        conn.close()


def test_publishing_with_nobody_listening_is_a_no_op():
    """The indicator and injector publish unconditionally; with no server that
    must be free and silent, not an error."""
    assert control.active() is None
    control.publish({"event": "state", "state": "listening"})
    control.publish_level(0.5)


def test_socket_is_private(server):
    srv, _engine = server
    assert stat.S_IMODE(srv.path.stat().st_mode) == 0o600
    assert stat.S_IMODE(srv.path.parent.stat().st_mode) == 0o700


def test_a_world_writable_directory_is_refused(tmp_path):
    """Someone who can write the directory can swap the socket for their own,
    and then every `mynah watch` in the session is talking to them."""
    shared = tmp_path / "shared"
    shared.mkdir(mode=0o777)
    os.chmod(shared, 0o777)
    with pytest.raises(PermissionError):
        control._prepare_dir(shared / "control.sock")


def test_a_stale_socket_is_replaced(tmp_path):
    """A killed mynah leaves the socket file behind; the next one must bind."""
    path = tmp_path / "run" / "control.sock"
    path.parent.mkdir(mode=0o700)
    path.touch()
    engine = FakeEngine()
    srv = control.ControlServer(
        on_toggle=engine.toggle, on_start=engine.start, on_stop=engine.stop,
        on_quit=engine.quit, state=lambda: engine.state, path=path,
    )
    srv.start()
    try:
        assert _talk(path, "status")[0]["ok"] is True
    finally:
        srv.stop()


def test_a_live_socket_is_not_stolen(server):
    """Two mynahs must not fight over one socket: the second refuses to start."""
    srv, engine = server
    second = control.ControlServer(
        on_toggle=engine.toggle, on_start=engine.start, on_stop=engine.stop,
        on_quit=engine.quit, state=lambda: engine.state, path=srv.path,
    )
    with pytest.raises(RuntimeError, match="already running"):
        second.start()


def test_stop_removes_the_socket(tmp_path):
    engine = FakeEngine()
    srv = control.ControlServer(
        on_toggle=engine.toggle, on_start=engine.start, on_stop=engine.stop,
        on_quit=engine.quit, state=lambda: engine.state,
        path=tmp_path / "run" / "control.sock",
    )
    srv.start()
    srv.stop()
    assert not srv.path.exists()
    assert control.active() is None


def test_socket_path_prefers_the_runtime_dir(monkeypatch):
    monkeypatch.delenv("MYNAH_SOCKET", raising=False)
    monkeypatch.setenv("XDG_RUNTIME_DIR", "/run/user/4242")
    assert control.socket_path() == Path("/run/user/4242/mynah/control.sock")
    monkeypatch.delenv("XDG_RUNTIME_DIR")
    assert control.socket_path() == Path(f"/tmp/mynah-{os.getuid()}/control.sock")


def test_client_says_how_to_start_one(tmp_path, monkeypatch):
    monkeypatch.setenv("MYNAH_SOCKET", str(tmp_path / "nothing.sock"))
    with pytest.raises(control.ControlError, match="no running mynah"):
        control.command("status")


# ---------------------------------------------------------------------------
# wtype injector
# ---------------------------------------------------------------------------


def test_text_goes_in_on_stdin_never_in_argv():
    """argv is world-readable through /proc: a transcript passed as an
    argument would be published to every process on the machine."""
    injector = linux_inject.WtypeInjector(binary="/usr/bin/wtype")
    with mock.patch("subprocess.run") as run:
        run.return_value = mock.Mock(returncode=0, stderr="")
        injector.type_text("the fee is computed on gross")
    argv, kwargs = run.call_args[0][0], run.call_args[1]
    assert argv == ["/usr/bin/wtype", "-"]
    assert kwargs["input"] == "the fee is computed on gross"
    assert "the fee" not in " ".join(argv)


def test_typing_publishes_what_landed(tmp_path):
    engine = FakeEngine()
    srv = control.ControlServer(
        on_toggle=engine.toggle, on_start=engine.start, on_stop=engine.stop,
        on_quit=engine.quit, state=lambda: engine.state,
        path=tmp_path / "run" / "control.sock",
    )
    srv.start()
    try:
        published: list[dict] = []
        with mock.patch.object(srv, "publish", published.append), \
             mock.patch("subprocess.run", return_value=mock.Mock(returncode=0, stderr="")):
            linux_inject.WtypeInjector(binary="/usr/bin/wtype").type_text("hello")
        assert {"event": "text", "text": "hello"} in published
    finally:
        srv.stop()


def test_a_failed_wtype_publishes_nothing(tmp_path):
    """The event means "this text reached the window", so a failure is silent."""
    with mock.patch("subprocess.run", return_value=mock.Mock(returncode=1, stderr="no seat")), \
         mock.patch.object(control, "publish") as publish:
        linux_inject.WtypeInjector(binary="/usr/bin/wtype").type_text("hello")
    publish.assert_not_called()


def test_empty_text_does_not_spawn_wtype():
    with mock.patch("subprocess.run") as run:
        linux_inject.WtypeInjector(binary="/usr/bin/wtype").type_text("")
    run.assert_not_called()


def test_missing_wtype_is_a_named_remedy(monkeypatch):
    monkeypatch.setenv("WAYLAND_DISPLAY", "wayland-0")
    monkeypatch.setattr(linux_inject, "find_binary", lambda: None)
    ok, hint = linux_inject.WtypeInjector().check_permissions(prompt=False)
    assert ok is False
    assert "pacman -S wtype" in hint


def test_an_x11_session_is_refused_with_the_reason(monkeypatch):
    monkeypatch.delenv("WAYLAND_DISPLAY", raising=False)
    monkeypatch.setenv("XDG_SESSION_TYPE", "x11")
    ok, hint = linux_inject.WtypeInjector(binary="/usr/bin/wtype").check_permissions(prompt=False)
    assert ok is False
    assert "Wayland" in hint


# ---------------------------------------------------------------------------
# whisper.cpp provider
# ---------------------------------------------------------------------------


def test_a_bare_size_resolves_to_a_ggml_file(tmp_path, monkeypatch):
    monkeypatch.setenv("MYNAH_MODEL_DIR", str(tmp_path))
    (tmp_path / "ggml-small.bin").write_bytes(b"x")
    assert linux_stt.find_model("small") == tmp_path / "ggml-small.bin"
    assert linux_stt.find_model("ggml-small.bin") == tmp_path / "ggml-small.bin"
    assert linux_stt.find_model("") == tmp_path / "ggml-small.bin"  # the default


def test_a_path_is_taken_as_given(tmp_path, monkeypatch):
    monkeypatch.setenv("MYNAH_MODEL_DIR", str(tmp_path / "elsewhere"))
    model = tmp_path / "my-own.bin"
    model.write_bytes(b"x")
    assert linux_stt.find_model(str(model)) == model


def test_a_missing_model_names_the_download(tmp_path, monkeypatch):
    monkeypatch.setenv("MYNAH_MODEL_DIR", str(tmp_path))
    provider = linux_stt.WhisperCppProvider(binary="/usr/bin/whisper-cli")
    with pytest.raises(RuntimeError) as excinfo:
        provider.load()
    message = str(excinfo.value)
    assert "curl" in message and "ggml-small.bin" in message
    assert provider.is_loaded is False


def test_a_missing_binary_names_the_package(tmp_path, monkeypatch):
    monkeypatch.setenv("MYNAH_MODEL_DIR", str(tmp_path))
    monkeypatch.setenv("MYNAH_WHISPER_CLI", "/nowhere/whisper-cli")
    with pytest.raises(RuntimeError, match="pacman -S whisper-cpp"):
        linux_stt.WhisperCppProvider().load()


def test_transcribe_passes_the_tuning_thresholds(tmp_path, monkeypatch):
    """The contract in tuning/tuning.toml is only shared if it is passed."""
    np = pytest.importorskip("numpy")
    monkeypatch.setenv("MYNAH_MODEL_DIR", str(tmp_path))
    monkeypatch.setenv("XDG_RUNTIME_DIR", str(tmp_path))
    model = tmp_path / "ggml-small.bin"
    model.write_bytes(b"x")
    provider = linux_stt.WhisperCppProvider(binary="/usr/bin/whisper-cli")
    provider.load()
    with mock.patch("subprocess.run") as run:
        run.return_value = mock.Mock(returncode=0, stdout=" the fee is computed\n", stderr="")
        text = provider.transcribe(np.zeros(16000, dtype=np.float32), 16000, "uk", "a prompt")
    argv = run.call_args[0][0]
    assert text == "the fee is computed"
    assert argv[:3] == ["/usr/bin/whisper-cli", "-m", str(model)]
    assert "-nth" in argv and argv[argv.index("-nth") + 1] == str(linux_stt.NO_SPEECH_THRESHOLD)
    assert "-lpt" in argv and argv[argv.index("-lpt") + 1] == str(linux_stt.LOGPROB_THRESHOLD)
    assert argv[argv.index("-l") + 1] == "uk"
    assert argv[argv.index("--prompt") + 1] == "a prompt"


def test_the_utterance_wav_is_deleted(tmp_path, monkeypatch):
    """The audio is the user's voice; it lives on tmpfs and not for long."""
    np = pytest.importorskip("numpy")
    monkeypatch.setenv("MYNAH_MODEL_DIR", str(tmp_path))
    monkeypatch.setenv("XDG_RUNTIME_DIR", str(tmp_path))
    (tmp_path / "ggml-small.bin").write_bytes(b"x")
    provider = linux_stt.WhisperCppProvider(binary="/usr/bin/whisper-cli")
    seen = {}

    def capture(argv, **_kwargs):
        wav = Path(argv[argv.index("-f") + 1])
        seen["path"] = wav
        seen["existed"] = wav.exists()
        seen["bytes"] = wav.stat().st_size
        return mock.Mock(returncode=0, stdout="", stderr="")

    with mock.patch("subprocess.run", capture):
        provider.transcribe(np.zeros(8000, dtype=np.float32), 16000, "ru", "")
    assert seen["existed"] and seen["bytes"] > 0
    assert not seen["path"].exists()


def test_non_speech_markers_are_not_typed():
    assert linux_stt._clean(" [BLANK_AUDIO]\n") == ""
    assert linux_stt._clean(" (wind blowing)\n") == ""
    assert linux_stt._clean(" one\n two\n") == "one two"


def test_a_failed_run_drops_the_utterance_rather_than_raising(tmp_path, monkeypatch):
    np = pytest.importorskip("numpy")
    monkeypatch.setenv("MYNAH_MODEL_DIR", str(tmp_path))
    monkeypatch.setenv("XDG_RUNTIME_DIR", str(tmp_path))
    (tmp_path / "ggml-small.bin").write_bytes(b"x")
    provider = linux_stt.WhisperCppProvider(binary="/usr/bin/whisper-cli")
    with mock.patch("subprocess.run", return_value=mock.Mock(returncode=3, stdout="", stderr="boom")):
        assert provider.transcribe(np.zeros(8000, dtype=np.float32), 16000, "ru", "") == ""


# ---------------------------------------------------------------------------
# Registry and the systemd unit
# ---------------------------------------------------------------------------


def test_linux_auto_detect_prefers_a_provider_that_is_installed():
    from mynah import providers

    table = {"absent": ("linux", lambda: "absent"), "present": ("linux", lambda: "present")}
    with mock.patch.dict(providers._AVAILABLE,
                         {"absent": lambda: False, "present": lambda: True}, clear=False):
        assert providers._platform_default("linux", table) == "present"


def test_auto_detect_falls_back_so_the_error_names_the_fix():
    """With nothing installed, returning None would say "no provider"; the
    first candidate's constructor says which package to install instead."""
    from mynah import providers

    table = {"whisper-cpp": ("linux", lambda: "x")}
    with mock.patch.dict(providers._AVAILABLE, {"whisper-cpp": lambda: False}, clear=False):
        assert providers._platform_default("linux", table) == "whisper-cpp"


def test_the_unit_is_tied_to_the_graphical_session():
    """mynah types through the compositor: started before there is one it can
    only fail, and left running after it there is nobody to type for."""
    from mynah import systemd

    unit = systemd.build_unit()
    assert "PartOf=graphical-session.target" in unit
    assert "WantedBy=graphical-session.target" in unit
    assert "Environment=MYNAH_SERVICE=1" in unit
    assert "StartLimitBurst=3" in unit


def test_the_unit_carries_a_custom_config_dir(monkeypatch):
    from mynah import systemd

    monkeypatch.setenv("MYNAH_CONFIG_DIR", "/home/someone/.config/mynah-test")
    assert "Environment=MYNAH_CONFIG_DIR=/home/someone/.config/mynah-test" in systemd.build_unit()


def test_install_writes_the_unit_and_enables_it(tmp_path, monkeypatch):
    from mynah import systemd

    monkeypatch.setattr(systemd, "_UNIT_DIR", tmp_path / "systemd" / "user")
    calls: list[list[str]] = []
    monkeypatch.setattr(systemd, "_systemctl",
                        lambda *args: calls.append(list(args)) or mock.Mock(returncode=0, stdout="", stderr=""))
    monkeypatch.setattr(systemd.shutil, "which", lambda _n: "/usr/bin/systemctl")
    assert systemd.install() == 0
    assert (tmp_path / "systemd" / "user" / "mynah.service").exists()
    assert ["daemon-reload"] in calls
    assert ["enable", "--now", "mynah.service"] in calls


def test_status_is_not_ok_when_the_unit_is_missing(tmp_path, monkeypatch):
    from mynah import systemd

    monkeypatch.setattr(systemd, "_UNIT_DIR", tmp_path / "none")
    assert systemd.status() == 1
