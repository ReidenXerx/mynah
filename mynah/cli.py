"""mynah — say it, and it types where you are.

    mynah                  start dictating: the hotkey opens and closes a session
    mynah setup            first run: dependencies, permissions, hotkey, service
    mynah config           what it is set to
    mynah set KEY=VALUE    change one setting (mynah set hotkey=<f8>)
    mynah service ...      install | uninstall | status of the login service
    mynah providers        which speech, typing and indicator providers are here

Every command takes --help. The one you run most is the first one, and after
`mynah setup` installs the service you rarely run any of them: the hotkey is the
interface.
"""

from __future__ import annotations

import argparse
import logging
import sys

from mynah import __version__, config as cfg, ui

# Friendly names for `mynah set`, including the obvious aliases. The stored key
# is on the right; several spellings reach the same setting because people
# reasonably guess "lang", "key" or "sensitivity".
FRIENDLY: dict[str, str] = {
    "hotkey": "hotkey", "key": "hotkey",
    "trigger": "trigger", "mode": "trigger",
    "language": "language", "lang": "language",
    "model": "model",
    "prompt": "prompt",
    "idle_timeout": "idle_timeout", "idle": "idle_timeout", "timeout": "idle_timeout",
    "vad": "vad",
    "auto_stop_silence": "auto_stop_silence", "silence": "auto_stop_silence",
    "show_indicator": "show_indicator", "indicator": "show_indicator",
    "idle_visible": "idle_visible", "idle_badge": "idle_visible",
    "menu_bar": "menu_bar", "menubar": "menu_bar",
    "frame_energy": "frame_energy", "sensitivity": "frame_energy",
    "min_energy": "min_energy",
    "min_utterance": "min_utterance",
    "stt_provider": "stt_provider", "stt": "stt_provider",
    "injector": "injector",
    "indicator_provider": "indicator",
}

FIELDS: list[tuple[str, str, str]] = [
    ("hotkey", "Hotkey", "Global hotkey, pynput syntax (e.g. <ctrl>+<space>)"),
    ("trigger", "Trigger", "toggle (press to start and stop) or ptt (hold to talk)"),
    ("language", "Language", "Spoken language code"),
    ("model", "Model", "Speech model repo or path (empty = the provider's default)"),
    ("prompt", "Prompt", "initial_prompt to bias recognition (empty = built-in)"),
    ("idle_timeout", "Idle timeout", "Seconds before the model unloads (0 = never)"),
    ("auto_stop_silence", "Auto-stop", "Seconds of silence that end a session (0 = off)"),
    ("vad", "VAD", "Split speech into utterances, so text appears as you pause"),
    ("show_indicator", "Indicator", "The floating mic indicator"),
    ("idle_visible", "Idle badge", "Keep the indicator dimmed-visible between sessions"),
    ("menu_bar", "Menu bar", "Menu bar or tray item with start, stop and quit"),
    ("frame_energy", "Frame energy", "Per-frame floor for speech (lower = more sensitive)"),
    ("min_energy", "Min energy", "Quietest utterance worth transcribing"),
    ("min_utterance", "Min utterance", "Shortest utterance worth transcribing, in seconds"),
    ("stt_provider", "Speech", "Force a speech provider (empty = auto)"),
    ("injector", "Typing", "Force a text injector (empty = auto)"),
    ("indicator", "Indicator provider", "Force an indicator (empty = auto)"),
]

TRIGGERS = ("toggle", "ptt")

# Settings whose valid values are the providers actually present. Empty stays
# valid everywhere: it means auto-detect.
PROVIDER_KEYS = {"stt_provider": "stt", "injector": "injector", "indicator": "indicator"}


def _validate(key: str, value: object) -> None:
    if key == "trigger" and value not in TRIGGERS:
        raise SystemExit(f"trigger is one of {' or '.join(TRIGGERS)}, got {value!r}")
    kind = PROVIDER_KEYS.get(key)
    if kind is None or value == "":
        return
    from mynah.providers import list_providers

    names = {name for name, _, _ in list_providers()[kind]}
    if value not in names:
        raise SystemExit(
            f"{key}={value!r} is not a provider here. Known: "
            f"{', '.join(sorted(names))} — or empty for auto-detect."
        )


def _needs_runtime() -> None:
    """Fail with an install hint rather than an ImportError traceback."""
    try:
        import sounddevice  # noqa: F401
        import pynput  # noqa: F401
    except ImportError:
        platform = "macos" if sys.platform == "darwin" else "linux"
        raise SystemExit(
            f"Mynah's speech runtime is not installed. Add it with:\n"
            f"  pipx inject mynah 'mynah[{platform}]'\n\n"
            + (
                "Then grant Accessibility and Microphone in System Settings → "
                "Privacy & Security."
                if platform == "macos"
                else "Then run: mynah setup"
            )
        )


def cmd_run(args: argparse.Namespace) -> int:
    """Listen for the hotkey and type what is said into the focused window."""
    config = cfg.load()
    _needs_runtime()
    from mynah import run_dictate

    try:  # a friendlier name than "python" in a process list; never fatal
        import setproctitle

        setproctitle.setproctitle("mynah")
    except Exception:
        pass

    # The service redirects stdout and stderr to its log, and these messages are
    # the only way to see why a session behaved as it did.
    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        stream=sys.stderr,
    )

    overrides: dict[str, object] = {
        "model": args.model or "",
        "language": args.language or "",
        "prompt": args.prompt if args.prompt is not None else "",
        "hotkey": args.hotkey or "",
    }
    if args.trigger:
        overrides["trigger"] = args.trigger
    if args.idle_timeout is not None:
        overrides["idle_timeout"] = args.idle_timeout
    if args.auto_stop_silence is not None:
        overrides["auto_stop_silence"] = args.auto_stop_silence
    if args.no_indicator:
        overrides["show_indicator"] = False
    return run_dictate(config, **overrides)


def cmd_setup(args: argparse.Namespace) -> int:
    """Walk the first run: dependencies, permissions, hotkey, login service."""
    from mynah import preflight

    return preflight.setup(install_service=not args.no_service)


def cmd_config(args: argparse.Namespace) -> int:
    config = cfg.load()
    ui.header("settings")
    rows = [[label, _shown(getattr(config, key)), description] for key, label, description in FIELDS]
    ui.table(str(cfg.CONFIG_PATH), [("Setting", "left"), ("Value", "left"), ("What it does", "left")], rows)
    ui.muted("\nChange one with:  mynah set <key>=<value>     e.g.  mynah set hotkey=<f8>")
    return 0


def _shown(value: object) -> str:
    if isinstance(value, bool):
        return "on" if value else "off"
    if value == "":
        return "(default)"
    return str(value)


def _coerce(raw: str, current: object) -> object:
    if isinstance(current, bool):
        if raw.lower() in ("on", "true", "yes", "1"):
            return True
        if raw.lower() in ("off", "false", "no", "0"):
            return False
        raise SystemExit(f"Expected on or off, got {raw!r}")
    if isinstance(current, float):
        try:
            return float(raw)
        except ValueError:
            raise SystemExit(f"Expected a number, got {raw!r}") from None
    if isinstance(current, int):
        try:
            return int(raw)
        except ValueError:
            raise SystemExit(f"Expected a whole number, got {raw!r}") from None
    return raw


def cmd_set(args: argparse.Namespace) -> int:
    if "=" not in args.assignment:
        raise SystemExit("Expected KEY=VALUE, e.g. mynah set hotkey=<f8>")
    name, _, raw = args.assignment.partition("=")
    name = name.strip().lower()
    if name not in FRIENDLY:
        raise SystemExit(f"Unknown setting {name!r}. Known: {', '.join(sorted(set(FRIENDLY)))}")
    key = FRIENDLY[name]
    config = cfg.load()
    value = _coerce(raw.strip(), getattr(config, key))
    _validate(key, value)
    setattr(config, key, value)
    path = cfg.save(config)
    ui.status(f"{key} = {value!r}", kind="ok")
    ui.muted(f"saved to {path}")
    return 0


def cmd_service(args: argparse.Namespace) -> int:
    """Install, remove or check the service that starts Mynah at login."""
    from mynah import service

    if args.service_action == "install":
        # Refuse if the runtime is missing: the service would exit non-zero on
        # startup and be restarted forever, filling the log with the same error.
        try:
            _needs_runtime()
        except SystemExit as e:
            print(
                f"{e}\n\nNot installing the service: without the runtime it would exit at "
                "startup and be restarted in a crash-loop, writing the same error to its "
                "log forever. Install the runtime, then run: mynah service install",
                file=sys.stderr,
            )
            return 1
        return service.install()
    if args.service_action == "uninstall":
        return service.uninstall()
    return service.status()


def cmd_providers(args: argparse.Namespace) -> int:
    """What can listen, type and show an indicator on this machine."""
    from mynah.providers import list_providers

    found = list_providers()
    ui.header("providers")
    for kind, title in (("stt", "speech"), ("injector", "typing"), ("indicator", "indicator")):
        rows = [[name, supports, "yes" if current else ""] for name, supports, current in found[kind]]
        ui.table(title, [("Name", "left"), ("Platform", "left"), ("In use", "right")], rows)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="mynah",
        description="Say it, and it types where you are.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--version", action="version", version=f"mynah {__version__}")
    parser.add_argument("--model", default="", help="Speech model for this run")
    parser.add_argument("--language", default="", help="Spoken language code for this run")
    parser.add_argument("--prompt", default=None, help="initial_prompt for this run")
    parser.add_argument("--hotkey", default="", help="Hotkey for this run, pynput syntax")
    parser.add_argument("--trigger", default="", choices=["", *TRIGGERS],
                        help="toggle (press to start and stop) or ptt (hold to talk)")
    parser.add_argument("--idle-timeout", type=float, default=None,
                        help="Seconds before the model unloads (0 = never)")
    parser.add_argument("--auto-stop-silence", type=float, default=None,
                        help="Seconds of silence that end a session (0 = off)")
    parser.add_argument("--no-indicator", action="store_true", help="Run without the floating indicator")
    parser.add_argument("--debug", action="store_true", help="Log every segmentation decision")
    parser.set_defaults(func=cmd_run)

    sub = parser.add_subparsers(title="commands", metavar="")

    setup = sub.add_parser("setup", aliases=["doctor"], help="First run: dependencies, permissions, hotkey, service")
    setup.add_argument("--no-service", action="store_true", help="Check everything, but do not install the login service")
    setup.set_defaults(func=cmd_setup)

    sub.add_parser("config", aliases=["cfg"], help="Show what Mynah is set to").set_defaults(func=cmd_config)

    setter = sub.add_parser("set", help="Change one setting (mynah set hotkey=<f8>)")
    setter.add_argument("assignment", metavar="KEY=VALUE")
    setter.set_defaults(func=cmd_set)

    service = sub.add_parser("service", aliases=["svc"], help="The login service: install, uninstall, status")
    service_sub = service.add_subparsers(dest="service_action", metavar="")
    service_sub.add_parser("install", help="Start Mynah at login and keep it running").set_defaults(
        func=cmd_service, service_action="install")
    service_sub.add_parser("uninstall", aliases=["remove"], help="Stop starting Mynah at login").set_defaults(
        func=cmd_service, service_action="uninstall")
    service_sub.add_parser("status", aliases=["st"], help="Is it installed and running?").set_defaults(
        func=cmd_service, service_action="status")
    service.set_defaults(func=cmd_service, service_action="status")

    sub.add_parser("providers", help="What can listen, type and show an indicator here").set_defaults(
        func=cmd_providers)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args) or 0)
    except RuntimeError as e:  # a corrupt config, and anything else with a clear message
        print(f"mynah: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
