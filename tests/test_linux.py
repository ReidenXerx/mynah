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
import subprocess
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


def test_a_failing_handler_does_not_break_the_socket(tmp_path):
    """The reply is an acknowledgement, so a handler that raises afterwards
    reaches subscribers as an event rather than as a broken connection."""
    def explode() -> None:
        raise RuntimeError("no microphone")

    srv = control.ControlServer(
        on_toggle=explode, on_start=explode, on_stop=explode, on_quit=explode,
        state=lambda: "idle", path=tmp_path / "s" / "control.sock",
    )
    srv.start()
    published: list[dict] = []
    try:
        with mock.patch.object(srv, "publish", published.append):
            reply = _talk(srv.path, "toggle")[0]
            deadline = time.monotonic() + 2
            while not published and time.monotonic() < deadline:
                time.sleep(0.01)
        assert reply == {"ok": True, "state": "idle"}
        assert published == [{"event": "error", "command": "toggle", "error": "no microphone"}]
        # And the socket still works afterwards.
        assert _talk(srv.path, "status")[0]["ok"] is True
    finally:
        srv.stop()


def test_a_slow_handler_does_not_hold_the_reply(tmp_path):
    """Ending a session drains the transcription queue — seconds of whisper.
    Holding the reply for that makes the key that stopped dictation look
    wedged, and the client times out with nothing to show for it."""
    started = threading.Event()
    release = threading.Event()

    def slow() -> None:
        started.set()
        release.wait(5)

    srv = control.ControlServer(
        on_toggle=slow, on_start=slow, on_stop=slow, on_quit=slow,
        state=lambda: "listening", path=tmp_path / "s" / "control.sock",
    )
    srv.start()
    try:
        began = time.monotonic()
        reply = _talk(srv.path, "stop")[0]
        answered_in = time.monotonic() - began
        assert reply["ok"] is True
        assert answered_in < 1.0, f"the reply waited {answered_in:.1f}s for the handler"
        assert started.wait(2), "the handler never ran"
    finally:
        release.set()
        srv.stop()


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
    # MYNAH_MODEL_DIR only prepends; the standard directories are still
    # searched, and this machine may well have a model in one of them.
    monkeypatch.setattr(linux_stt, "MODEL_DIRS", ())
    provider = linux_stt.WhisperCppProvider(binary="/usr/bin/whisper-cli")
    with pytest.raises(RuntimeError) as excinfo:
        provider.load()
    message = str(excinfo.value)
    assert "curl" in message and "ggml-small.bin" in message
    assert provider.is_loaded is False


def test_a_missing_binary_names_the_package(tmp_path, monkeypatch):
    monkeypatch.setenv("MYNAH_MODEL_DIR", str(tmp_path))
    monkeypatch.setattr(linux_stt, "MODEL_DIRS", ())
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


def test_the_configured_model_reaches_the_provider(tmp_path, monkeypatch):
    """`mynah set model=base` was silently ignored on Linux: the engine poked
    a private attribute that only the mlx provider had."""
    from mynah import config as cfg, engine as eng

    monkeypatch.setenv("MYNAH_MODEL_DIR", str(tmp_path))
    monkeypatch.setattr(linux_stt, "MODEL_DIRS", ())
    (tmp_path / "ggml-base.bin").write_bytes(b"x")

    provider = linux_stt.WhisperCppProvider(binary="/usr/bin/whisper-cli")
    config = cfg.Config()
    config.model = "base"
    monkeypatch.setattr(eng, "select_stt_provider", lambda _c: provider)
    monkeypatch.setattr(eng, "select_injector", lambda _c: mock.Mock())
    monkeypatch.setattr(eng, "select_indicator", lambda _c: mock.Mock())
    monkeypatch.setattr(eng.DictationEngine, "run", lambda self: 0)

    assert eng.run_dictate(config) == 0
    assert provider.model_ref == "base"
    provider.load()
    assert provider._model_path == tmp_path / "ggml-base.bin"


def test_a_lua_binding_is_recognised_by_its_description(monkeypatch):
    """The Omarchy plugin binds the key through `hl.bind`, and Hyprland then
    reports the dispatcher as "__lua" with an index for an argument — the
    command is nowhere in the binding. Its description is."""
    from mynah import preflight

    monkeypatch.setattr(preflight, "_hyprland_binds", lambda: [
        ("SUPER+ALT+D", "__lua 339", "Mynah: dictate"),
        ("SUPER+RETURN", "exec kitty", ""),
    ])
    result = preflight._check_compositor_hotkey()
    assert result.ok is True
    assert "SUPER+ALT+D" in result.detail


def test_no_binding_says_what_to_add(monkeypatch):
    from mynah import preflight

    monkeypatch.setattr(preflight, "_hyprland_binds", lambda: [("SUPER+RETURN", "exec kitty", "")])
    result = preflight._check_compositor_hotkey()
    assert result.ok is False
    assert "mynah toggle" in result.hint


def test_a_compositor_we_cannot_ask_is_not_a_failure(monkeypatch):
    """Only Hyprland can be asked. Everywhere else the check has no verdict,
    and a check that cannot run must not fail the user."""
    from mynah import preflight

    monkeypatch.setattr(preflight, "_hyprland_binds", lambda: None)
    assert preflight._check_compositor_hotkey().ok is True


def test_modmask_spells_the_combo():
    from mynah import preflight

    assert preflight._combo(72, "D") == "SUPER+ALT+D"
    assert preflight._combo(65, "M") == "SUPER+SHIFT+M"
    assert preflight._combo(0, "F8") == "F8"


# ---------------------------------------------------------------------------
# Typing into apps that ignore a virtual keyboard
# ---------------------------------------------------------------------------


def test_terminals_paste_with_ctrl_shift_v():
    """Ctrl+V is a control character to the program inside a terminal."""
    from mynah.providers import linux_clipboard as clip

    assert clip.paste_chord("dev.warp.warp") == ("CTRL SHIFT", "V")
    assert clip.paste_chord("foot") == ("CTRL SHIFT", "V")
    assert clip.paste_chord("org.gnome.texteditor") == ("CTRL", "V")
    assert clip.paste_chord("") == ("CTRL", "V")


def test_the_clipboard_is_borrowed_and_given_back(monkeypatch):
    from mynah.providers import linux_clipboard as clip

    injector = clip.ClipboardInjector()
    injector._copy = "/usr/bin/wl-copy"
    injector._paste = "/usr/bin/wl-paste"
    monkeypatch.setattr(clip, "focused_class", lambda: "dev.warp.warp")
    monkeypatch.setattr(clip, "RESTORE_AFTER", 0.01)
    monkeypatch.setattr(injector, "_send_chord", lambda mods, key: True)

    written: list[str] = []

    def fake_run(argv, **kwargs):
        if argv[0].endswith("wl-paste") and "--list-types" in argv:
            return mock.Mock(returncode=0, stdout="text/plain\n")
        if argv[0].endswith("wl-paste"):
            return mock.Mock(returncode=0, stdout="what was there before")
        written.append(kwargs.get("input", ""))
        return mock.Mock(returncode=0, stdout="")

    with mock.patch("subprocess.run", fake_run):
        injector.type_text("the fee is computed")
        deadline = time.monotonic() + 2
        while len(written) < 2 and time.monotonic() < deadline:
            time.sleep(0.01)

    assert written[0] == "the fee is computed"
    assert written[-1] == "what was there before", "the clipboard must be given back"


def test_wl_copy_output_is_not_captured():
    """wl-copy forks a daemon that serves the selection and inherits the pipes,
    so capturing its output waits for a process designed not to exit. The first
    version timed out on every utterance and silently fell back to typing."""
    from mynah.providers import linux_clipboard as clip

    injector = clip.ClipboardInjector()
    injector._copy = "/usr/bin/wl-copy"
    with mock.patch("subprocess.run", return_value=mock.Mock(returncode=0)) as run:
        assert injector._write_clipboard("hello") is True
    kwargs = run.call_args[1]
    assert kwargs.get("capture_output") is not True
    assert kwargs.get("stdout") is subprocess.DEVNULL
    assert kwargs.get("stderr") is subprocess.DEVNULL


def test_an_image_on_the_clipboard_is_left_alone():
    """A clipboard holding something that is not text cannot be carried, so it
    is not read and not restored — better untouched than replaced with text."""
    from mynah.providers import linux_clipboard as clip

    injector = clip.ClipboardInjector()
    injector._paste = "/usr/bin/wl-paste"
    with mock.patch("subprocess.run", return_value=mock.Mock(returncode=0, stdout="image/png\n")):
        assert injector._read_clipboard() is None


def test_the_chord_goes_through_the_compositor(monkeypatch):
    """Not through a virtual keyboard: an app that ignores those is the whole
    reason this injector exists."""
    from mynah.providers import linux_clipboard as clip

    injector = clip.ClipboardInjector()
    injector._hyprctl = "/usr/bin/hyprctl"
    monkeypatch.setenv("HYPRLAND_INSTANCE_SIGNATURE", "abc")
    with mock.patch("subprocess.run", return_value=mock.Mock(returncode=0, stdout="ok")) as run:
        assert injector._send_chord("CTRL SHIFT", "V") is True
    argv = run.call_args[0][0]
    assert argv[:2] == ["/usr/bin/hyprctl", "dispatch"]
    assert 'mods = "CTRL SHIFT"' in argv[2] and 'key = "V"' in argv[2]


def test_the_smart_injector_pastes_only_where_typing_fails(monkeypatch):
    from mynah.providers import linux_clipboard as clip

    injector = clip.SmartInjector()
    typed: list[str] = []
    pasted: list[str] = []
    monkeypatch.setattr(injector._typing, "type_text", typed.append)
    monkeypatch.setattr(injector._pasting, "type_text", pasted.append)

    monkeypatch.setattr(injector, "_class_now", lambda: "foot")
    injector.type_text("into a terminal that honours the keymap")
    monkeypatch.setattr(injector, "_class_now", lambda: "dev.warp.warp")
    injector.type_text("into warp")

    assert typed == ["into a terminal that honours the keymap"]
    assert pasted == ["into warp"]


def test_wtype_chord_presses_and_releases_in_order():
    injector = linux_inject.WtypeInjector(binary="/usr/bin/wtype")
    with mock.patch("subprocess.run", return_value=mock.Mock(returncode=0, stderr="")) as run:
        assert injector.send_chord("CTRL SHIFT", "V") is True
    assert run.call_args[0][0] == [
        "/usr/bin/wtype", "-M", "ctrl", "-M", "shift", "-P", "V", "-p", "V", "-m", "shift", "-m", "ctrl",
    ]


def test_the_listing_names_the_one_that_will_be_used():
    """Three injectors run on Linux; exactly one is chosen. A listing that
    marks all three "in use" answers neither question."""
    from mynah import config as cfg
    from mynah.providers import chosen_names

    config = cfg.Config()
    chosen = chosen_names(config)
    if sys.platform.startswith("linux"):
        assert chosen["injector"] == "smart"
        config.injector = "clipboard"
        assert chosen_names(config)["injector"] == "clipboard"
        # An unknown name cannot win: _validate rejects it at the CLI, and
        # here it falls back to the platform's choice rather than to nothing.
        config.injector = "nonsense"
        assert chosen_names(config)["injector"] == "smart"


# ---------------------------------------------------------------------------
# The spectrum behind the level meter
# ---------------------------------------------------------------------------


def _tone(np, hz, seconds=0.03, rate=16000, amplitude=0.2):
    t = np.arange(int(rate * seconds)) / rate
    return (amplitude * np.sin(2 * np.pi * hz * t)).astype(np.float32)


def test_bands_cover_speech_low_to_high():
    np = pytest.importorskip("numpy")
    from mynah.engine import _band_edges

    edges = _band_edges(np, 480, 16000)
    assert len(edges) == 12
    # Every band owns at least one bin — otherwise the low end is always empty.
    assert all(hi > lo for lo, hi in edges)
    # Low to high, and inside the frame.
    assert edges == sorted(edges)
    assert edges[-1][1] <= 241


def test_silence_is_flat_and_a_voice_is_not():
    """The first version divided unnormalised FFT magnitudes by a guessed
    reference, so every band saturated at 1 and the meter was a solid block —
    for silence as well."""
    np = pytest.importorskip("numpy")
    from mynah.engine import _band_edges, _spectrum

    window = np.hanning(480)
    edges = _band_edges(np, 480, 16000)

    silence = _spectrum(np.zeros(480, dtype=np.float32), np, window, edges)
    assert max(silence) == 0.0

    voice = _spectrum(_tone(np, 220) + _tone(np, 700, amplitude=0.1), np, window, edges)
    assert max(voice) > 0.5, "a voice should reach most of the meter"
    assert min(voice) < 0.2, "and should not light every band at once"


def test_a_tone_lands_in_the_band_it_belongs_to():
    np = pytest.importorskip("numpy")
    from mynah.engine import _band_edges, _spectrum

    window = np.hanning(480)
    edges = _band_edges(np, 480, 16000)

    low = _spectrum(_tone(np, 120), np, window, edges)
    high = _spectrum(_tone(np, 4000), np, window, edges)
    assert low.index(max(low)) < 3, "120 Hz belongs at the left end"
    assert high.index(max(high)) > 8, "4 kHz belongs at the right end"


def test_the_engine_only_measures_a_spectrum_when_something_draws_one():
    """An FFT per frame on the audio thread is cheap, but not free, and the
    macOS indicator shows one bar."""
    from mynah.providers.base import NullIndicator
    from mynah.providers.linux_indicator import SocketIndicator

    assert NullIndicator().wants_spectrum is False
    assert SocketIndicator.wants_spectrum is True


def test_the_bands_ride_with_the_level(tmp_path):
    """One event per frame, not two: the socket should not carry two messages
    30 times a second for one frame of audio."""
    engine = FakeEngine()
    srv = control.ControlServer(
        on_toggle=engine.toggle, on_start=engine.start, on_stop=engine.stop,
        on_quit=engine.quit, state=lambda: engine.state,
        path=tmp_path / "run" / "control.sock",
    )
    srv.start()
    try:
        published: list[dict] = []
        with mock.patch.object(srv, "publish", published.append):
            indicator = __import__(
                "mynah.providers.linux_indicator", fromlist=["SocketIndicator"]
            ).SocketIndicator()
            indicator.update_spectrum([0.1, 0.9, 0.4])
            indicator.update_level(0.5)
        assert published == [{"event": "level", "level": 0.5, "bands": [0.1, 0.9, 0.4]}]
    finally:
        srv.stop()


# ---------------------------------------------------------------------------
# Where the extra comes from
# ---------------------------------------------------------------------------


def test_the_extra_is_never_requested_by_bare_name(monkeypatch):
    """`mynah` on PyPI belongs to someone else — an unrelated 0.0.0 package.

    Asking pip for `mynah[linux]` resolves to that, downloads it and runs its
    build. The extra is requested from the same place the running code came
    from, or by naming its dependencies, and never by the bare name.
    """
    from mynah import preflight

    monkeypatch.setattr(preflight, "_origin", lambda: "git+https://github.com/ReidenXerx/mynah.git@abc123")
    spec = preflight._extra_spec()
    assert spec.endswith("@ git+https://github.com/ReidenXerx/mynah.git@abc123")

    calls: list[list[str]] = []
    monkeypatch.setattr(preflight.subprocess, "run",
                        lambda argv, **kw: calls.append(argv) or mock.Mock(returncode=0))
    preflight._inject_extra()
    assert calls[0][:3] == ["pipx", "inject", "mynah"]
    assert calls[0][3] == spec

    # With no recorded origin — a local or editable install — the extra's own
    # dependencies are named instead, so the `mynah` name is never resolved.
    calls.clear()
    monkeypatch.setattr(preflight, "_origin", lambda: "")
    preflight._inject_extra()
    assert all(not pkg.startswith("mynah") for pkg in calls[0][3:]), calls[0]
    assert "sounddevice" in calls[0]


def test_the_fallback_list_matches_pyproject():
    """A dependency added to an extra and forgotten here would be missing on a
    first run that has no install origin to ask."""
    import tomllib

    from mynah import preflight

    root = Path(__file__).resolve().parent.parent
    data = tomllib.loads((root / "pyproject.toml").read_text(encoding="utf-8"))
    extras = data["project"]["optional-dependencies"]
    for name, listed in preflight.EXTRA_PACKAGES.items():
        declared = {
            dep.split(">")[0].split("<")[0].split("=")[0].split("[")[0].strip()
            for dep in extras[name]
        }
        assert set(listed) == declared, f"{name}: {set(listed) ^ declared}"


def test_the_origin_is_only_trusted_when_it_is_a_url(monkeypatch):
    """direct_url.json can record a local directory; that is not something to
    hand to pip as a package spec."""
    from mynah import preflight

    class FakeDistribution:
        @staticmethod
        def from_name(_name):
            class D:
                @staticmethod
                def read_text(_file):
                    return '{"url": "file:///home/someone/Projects/mynah", "dir_info": {"editable": true}}'
            return D()

    monkeypatch.setitem(__import__("sys").modules, "importlib.metadata", FakeDistribution)
    # The real function imports Distribution inside, so patch the attribute it uses.
    import importlib.metadata as meta

    monkeypatch.setattr(meta, "Distribution", FakeDistribution)
    assert preflight._origin() == ""
