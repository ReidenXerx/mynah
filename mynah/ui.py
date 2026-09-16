"""Mynah's terminal output.

No dependency: a resident dictation daemon should not pull a rendering library
in to print six lines. Colour is switched off when stdout is not a TTY, when
NO_COLOR is set, or when TERM says dumb — so piping gives clean text.
"""

from __future__ import annotations

import os
import shutil
import sys

SAFFRON = "\033[38;5;214m"
INK = "\033[38;5;245m"
GREEN = "\033[38;5;72m"
RED = "\033[38;5;167m"
BOLD = "\033[1m"
OFF = "\033[0m"


def colour_enabled(stream=None) -> bool:
    stream = stream or sys.stdout
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("TERM", "") == "dumb":
        return False
    return bool(getattr(stream, "isatty", lambda: False)())


def paint(text: str, colour: str, stream=None) -> str:
    return f"{colour}{text}{OFF}" if colour_enabled(stream) else text


def header(subtitle: str = "") -> None:
    """The mark, the name, and what this invocation is about."""
    name = paint("mynah", SAFFRON + BOLD)
    tail = f" {paint('·', INK)} {subtitle}" if subtitle else ""
    print(f"\n{name}{tail}")
    print(paint("─" * min(shutil.get_terminal_size((72, 20)).columns, 72), INK))


def status(message: str, kind: str = "info") -> None:
    mark = {"ok": (GREEN, "✓"), "bad": (RED, "✗"), "info": (SAFFRON, "▸")}.get(kind, (SAFFRON, "▸"))
    print(f"{paint(mark[1], mark[0])} {message}")


def muted(message: str) -> None:
    print(paint(message, INK))


def table(title: str, columns: list[tuple[str, str]], rows: list[list[str]]) -> None:
    """A plain aligned table. ``columns`` is (heading, "left"|"right")."""
    if title:
        print(f"\n{paint(title, INK)}")
    widths = [len(head) for head, _ in columns]
    for row in rows:
        for i, cell in enumerate(row[: len(columns)]):
            widths[i] = max(widths[i], len(str(cell)))
    head = "  ".join(
        (head.ljust(widths[i]) if align == "left" else head.rjust(widths[i]))
        for i, (head, align) in enumerate(columns)
    )
    print(paint(head, INK))
    for row in rows:
        cells = []
        for i, (_, align) in enumerate(columns):
            cell = str(row[i]) if i < len(row) else ""
            cells.append(cell.ljust(widths[i]) if align == "left" else cell.rjust(widths[i]))
        print("  ".join(cells).rstrip())
