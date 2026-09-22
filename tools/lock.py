#!/usr/bin/python3
"""Write the dependency locks, so what gets installed is decided here and not by PyPI later.

Mynah listens to a microphone and types into whatever has focus, so the packages it imports are
worth pinning to exact versions and verifying byte for byte. This writes two files:

  requirements/build.lock   the build backend, installed first and alone
  requirements/linux.lock   everything the Linux extra imports, transitively

Both are ordinary pip requirements files with --hash lines, meant to be installed with
--require-hashes, which makes pip refuse anything that is not listed.

    python3 tools/lock.py            write the files
    python3 tools/lock.py --check    fail if they are out of date (for CI)

Every hash a package may legitimately match is listed, not only the one for the machine this ran
on: pip then picks the artifact for the running interpreter and still verifies it. Raising a pin
is a deliberate edit of PINS here, followed by running this and reading the diff.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "requirements"
TIMEOUT = 30

# The build backend. Pinned separately because it has to be in place before anything is built.
BUILD = {"setuptools": "84.0.0"}

# Everything `pip install mynah[linux]` resolves to, transitively.
RUNTIME = {
    "sounddevice": "0.5.6",
    "webrtcvad-wheels": "2.0.14",
    "numpy": "2.5.3",
    "cffi": "2.1.1",
    "pycparser": "3.0",
}

# What the locks cover. An interpreter or machine outside this set matches no artifact, so pip
# stops with "no matching distribution" rather than installing something nobody verified.
PY_TAGS = ("cp311", "cp312", "cp313", "cp314", "py3")
MACHINES = ("x86_64", "aarch64")
LIBCS = ("manylinux", "musllinux")

# webrtcvad-wheels publishes wheels only up to cp313, and Arch ships CPython 3.14. Its source
# archive is allowed -- by name, never in general -- and is hash-checked like every wheel here.
SDIST_ALLOWED = {"webrtcvad-wheels"}

BUILD_HEADER = """# The build backend, pinned and hash-checked. Written by tools/lock.py -- edit that, not this.
#
# It is installed on its own, before anything else, because one package in linux.lock has to be
# compiled from a source archive and pip's build isolation would otherwise fetch a build backend
# of its own choosing without checking it. With this in place and --no-build-isolation, the only
# setuptools that can run is this one.
#
#   pip install --require-hashes --only-binary :all: -r requirements/build.lock
"""

RUNTIME_HEADER = """# Every artifact the Linux extra may install, pinned by version and verified by hash.
# Written by tools/lock.py -- edit that, not this.
#
#   pip install --require-hashes --only-binary :all: \\
#               --no-binary webrtcvad-wheels --no-build-isolation \\
#               -r requirements/linux.lock
#
# Covers CPython 3.11 to 3.14 on x86_64 and aarch64 Linux, glibc and musl alike. Every hash a
# package may legitimately match is listed, so pip picks the artifact for the running interpreter
# and still verifies it; an interpreter or machine outside that set matches nothing here and pip
# stops rather than installing something unverified.
#
# One exception, deliberate and narrow: webrtcvad-wheels 2.0.14 publishes wheels only up to
# cp313 and Arch ships CPython 3.14, so its source archive is permitted -- by name, not in
# general. It is hash-checked exactly like every wheel here, it is the only package built
# locally, and it builds against the setuptools pinned in build.lock.
"""


def artifacts(name: str, version: str) -> list[dict]:
    """The files on PyPI for this release that these locks are willing to cover."""
    url = f"https://pypi.org/pypi/{name}/{version}/json"
    with urllib.request.urlopen(url, timeout=TIMEOUT) as response:
        release = json.load(response)
    kept = []
    for f in release["urls"]:
        if f["packagetype"] == "sdist":
            if name in SDIST_ALLOWED:
                kept.append(f)
            continue
        filename = f["filename"]
        if not any("-" + tag + "-" in filename for tag in PY_TAGS):
            continue
        if filename.endswith("-none-any.whl"):       # pure Python: every machine
            kept.append(f)
        elif any(m in filename for m in MACHINES) and any(l in filename for l in LIBCS):
            kept.append(f)
    return kept


def block(name: str, version: str) -> str:
    files = artifacts(name, version)
    if not files:
        sys.exit(f"{name} {version}: nothing on PyPI matches what these locks cover")
    hashes = sorted({f["digests"]["sha256"] for f in files})
    lines = [f"{name}=={version} \\"]
    lines += [f"    --hash=sha256:{h} \\" for h in hashes[:-1]]
    lines.append(f"    --hash=sha256:{hashes[-1]}")
    return "\n".join(lines)


def render(header: str, pins: dict[str, str]) -> str:
    return header + "\n" + "\n".join(block(n, v) for n, v in pins.items()) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the files are out of date")
    args = parser.parse_args(argv[1:])

    wanted = {OUT / "build.lock": render(BUILD_HEADER, BUILD),
              OUT / "linux.lock": render(RUNTIME_HEADER, RUNTIME)}
    stale = []
    for path, text in wanted.items():
        current = path.read_text(encoding="utf-8") if path.is_file() else ""
        if current == text:
            continue
        stale.append(path.name)
        if not args.check:
            path.parent.mkdir(exist_ok=True)
            path.write_text(text, encoding="utf-8")

    if args.check and stale:
        print("out of date, run tools/lock.py: " + ", ".join(stale))
        return 1
    print("up to date" if not stale else "wrote " + ", ".join(stale))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
