"""Speech on Linux: whisper.cpp through its `whisper-cli` binary.

macOS gets mlx-whisper, which runs in-process on the Apple GPU. Linux has no
single equivalent, and the honest default on a distro is the binary the distro
already packages: `whisper-cli` from whisper.cpp (`pacman -S whisper-cpp`,
`apt install whisper.cpp`, or the vendored build in `vendor/whisper.cpp`).

That makes this provider a subprocess per utterance rather than a resident
model, which sounds worse than it is: whisper.cpp mmaps the model file, so
after the first run the weights are in the page cache and start-up is a few
hundred milliseconds. What it costs is real but bounded; what it buys is a
Linux install with no Python speech dependency at all.

``is_loaded`` therefore means "the model file has been found and read at least
once", and ``unload`` drops that claim. There is no resident allocation for the
idle timer to free — on this platform the page cache is the idle policy.

The audio never reaches the disk: the temporary WAV is written into the same
0700 tmpfs directory as the control socket and unlinked as soon as
`whisper-cli` exits.
"""

from __future__ import annotations

import logging
import hashlib
import os
import shutil
import subprocess
import tempfile
import wave
from pathlib import Path
from typing import TYPE_CHECKING

from mynah.providers.base import STTProvider

if TYPE_CHECKING:  # pragma: no cover - typing only
    import numpy as np

logger = logging.getLogger("mynah.stt.whispercpp")

# The thresholds the tuning contract pins (tuning/tuning.toml). The Swift app
# sets the same pair on whisper.cpp; passing them here is what makes "the same
# segmentation contract on every platform" true rather than aspirational.
NO_SPEECH_THRESHOLD = 0.35
LOGPROB_THRESHOLD = -0.5

# Binary names in the order we try them. Upstream renamed `main` to
# `whisper-cli` in 1.7.2; distro packages have followed, but a hand-built
# checkout on a user's machine may still be the old name.
BINARIES = ("whisper-cli", "whisper-cpp", "whisper", "main")

# Where mynah puts a model it was told to download: under the user's data
# directory, because a model is data, and because nothing there needs root.
DEFAULT_MODEL_DIR = "~/.local/share/mynah/models"

# Where a ggml model may be sitting. Ordered most-specific first.
MODEL_DIRS = (
    DEFAULT_MODEL_DIR,
    "~/.cache/whisper.cpp",
    "/usr/share/whisper.cpp/models",
    "/usr/share/whisper.cpp",
)

DEFAULT_MODEL = "small"

# The download the setup check offers. Sizes are the published ggml sizes, used
# only to tell the user what they are about to pull.
MODEL_SIZES = {"tiny": "75 MB", "base": "142 MB", "small": "466 MB", "medium": "1.5 GB", "large-v3": "2.9 GB"}
# Pinned to one commit of the model repository, not to `main`.
#
# `resolve/main` is a moving target: whatever is behind it today is not
# necessarily what was reviewed, and the file it returns is handed straight to
# whisper-cli, which parses it as a native binary format. So the revision is
# fixed here and the published SHA-256 of each file is checked after the
# download. The hashes are the repository's own LFS object ids, which are the
# SHA-256 of the file contents; ggml-small.bin was verified against a copy
# downloaded before this pin.
#
# To move the pin: pick a commit from
# https://huggingface.co/ggerganov/whisper.cpp/commits/main, then take the new
# hashes from https://huggingface.co/api/models/ggerganov/whisper.cpp?blobs=true
# (each sibling's `lfs.oid`). Both change together, deliberately, or not at all.
MODEL_REVISION = "5359861c739e955e79d9a303bcbc70fb988958b1"
MODEL_URL = ("https://huggingface.co/ggerganov/whisper.cpp/resolve/"
             + MODEL_REVISION + "/ggml-{name}.bin")

MODEL_SHA256 = {
    "tiny": "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21",
    "base": "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe",
    "small": "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b",
    "medium": "6c14d5adee5f86394037b4e4e8b59f1673b6cee10e3cf0b11bbdbee79c156208",
    "large-v3": "64d182b440b98d5203c4f9bd541544d84c605196c4f7b845dfa11fb23594d1e2",
}


def find_binary() -> str | None:
    """The `whisper-cli` binary, or None if whisper.cpp is not installed."""
    override = os.environ.get("MYNAH_WHISPER_CLI")
    if override:
        return override if Path(override).exists() else None
    for name in BINARIES:
        found = shutil.which(name)
        if found:
            return found
    return None


def model_dirs() -> list[Path]:
    dirs = []
    override = os.environ.get("MYNAH_MODEL_DIR")
    if override:
        dirs.append(Path(override).expanduser())
    dirs.extend(Path(d).expanduser() for d in MODEL_DIRS)
    return dirs


def find_model(name: str = "") -> Path | None:
    """Resolve a model name or path to a ggml file on disk.

    ``name`` is whatever the user put in ``model``: a full path, a file name
    (``ggml-small.bin``) or a bare size (``small``). Empty means the default.
    """
    wanted = (name or DEFAULT_MODEL).strip()
    as_path = Path(wanted).expanduser()
    if as_path.is_file():
        return as_path
    stem = as_path.name
    candidates = [stem]
    if not stem.startswith("ggml-"):
        candidates.append(f"ggml-{stem}")
    candidates = [c if c.endswith(".bin") else f"{c}.bin" for c in candidates]
    for directory in model_dirs():
        for candidate in candidates:
            path = directory / candidate
            if path.is_file():
                return path
    return None


def download_command(name: str = DEFAULT_MODEL) -> str:
    """The exact command that fetches a model, for a hint the user can paste.

    It downloads from the pinned revision and then checks the file against its
    published hash, deleting it if it does not match — a model that is not the
    one we pinned must not be left sitting where whisper-cli will find it.
    """
    target = Path(DEFAULT_MODEL_DIR).expanduser() / f"ggml-{name}.bin"
    url = MODEL_URL.format(name=name)
    digest = MODEL_SHA256.get(name)
    if not digest:
        return f"curl -L --create-dirs -o {target} {url}"
    return (f"curl -L --create-dirs -o {target} {url} && "
            f"echo '{digest}  {target}' | sha256sum -c - || rm -f {target}")


def model_digest(path: Path) -> str:
    """The SHA-256 of a model file, read in pieces rather than all at once."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def model_is_known(path: Path, name: str = "") -> bool | None:
    """Whether this file is the model we pinned.

    True if it matches, False if it does not, and None if there is no published
    hash to compare against — somebody may legitimately point Mynah at a model
    of their own, and that is their business, not a failure.
    """
    stem = name or path.stem.removeprefix("ggml-")
    expected = MODEL_SHA256.get(stem)
    if not expected:
        return None
    try:
        return model_digest(path) == expected
    except OSError:
        return False


def _scratch_dir() -> Path:
    """A 0700 directory on tmpfs for the utterance WAV."""
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    base = Path(runtime) / "mynah" if runtime else Path(f"/tmp/mynah-{os.getuid()}")
    base.mkdir(mode=0o700, parents=True, exist_ok=True)
    return base


class WhisperCppProvider(STTProvider):
    """Transcribes an utterance by handing a temp WAV to `whisper-cli`."""

    name = "whisper-cpp"

    def __init__(self, model: str = "", binary: str | None = None) -> None:
        self.model_ref = model
        self._binary = binary
        self._model_path: Path | None = None

    # ---------- lifecycle ----------

    def load(self) -> None:
        """Resolve the binary and the model, and warm the page cache.

        Raises RuntimeError with a hint the user can act on, rather than
        failing later inside a session with a subprocess error.
        """
        binary = self._binary or find_binary()
        if binary is None:
            raise RuntimeError(
                "whisper.cpp is not installed — no `whisper-cli` on PATH.\n"
                "  Arch:   sudo pacman -S whisper-cpp\n"
                "  Debian: sudo apt install whisper.cpp\n"
                "  Or point mynah at a build: MYNAH_WHISPER_CLI=/path/to/whisper-cli"
            )
        self._binary = binary
        model = find_model(self.model_ref)
        if model is None:
            wanted = (self.model_ref or DEFAULT_MODEL).strip()
            size = MODEL_SIZES.get(wanted, "")
            raise RuntimeError(
                f"No whisper model named {wanted!r} in "
                f"{', '.join(MODEL_DIRS) or DEFAULT_MODEL_DIR}.\n"
                f"  Download it{f' ({size})' if size else ''}:\n"
                f"    {download_command(wanted if wanted in MODEL_SIZES else DEFAULT_MODEL)}\n"
                "  Or set a path:  mynah set model=/path/to/ggml-small.bin"
            )
        self._model_path = model
        # Touch the first megabyte so the first utterance doesn't pay for the
        # read. The rest arrives via mmap on the first transcribe.
        try:
            with model.open("rb") as fh:
                fh.read(1 << 20)
        except OSError:
            logger.debug("could not pre-read %s", model, exc_info=True)
        logger.info("whisper.cpp ready: %s (%s)", model.name, binary)

    @property
    def is_loaded(self) -> bool:
        return self._model_path is not None

    def unload(self) -> None:
        # Nothing is resident: the model is mmapped by a process that has
        # already exited. Dropping the path keeps `is_loaded` honest.
        self._model_path = None

    # ---------- transcription ----------

    def transcribe(
        self,
        audio: "np.ndarray",
        sample_rate: int,
        language: str,
        initial_prompt: str,
    ) -> str:
        if not self.is_loaded:
            self.load()
        assert self._model_path is not None and self._binary is not None

        fd, wav_path = tempfile.mkstemp(suffix=".wav", dir=_scratch_dir())
        os.close(fd)
        try:
            _write_wav(Path(wav_path), audio, sample_rate)
            argv = [
                self._binary,
                "-m", str(self._model_path),
                "-f", wav_path,
                "-l", language or "auto",
                "-nt",                      # no timestamps: we want the words
                "-np",                      # no progress chatter on stdout
                "-t", str(_threads()),
                "-nth", str(NO_SPEECH_THRESHOLD),
                "-lpt", str(LOGPROB_THRESHOLD),
            ]
            if initial_prompt:
                argv += ["--prompt", initial_prompt]
            logger.debug("whisper-cli %s", " ".join(argv[1:]))
            try:
                done = subprocess.run(
                    argv,
                    capture_output=True,
                    text=True,
                    timeout=_timeout_for(audio, sample_rate),
                )
            except subprocess.TimeoutExpired:
                logger.warning("whisper-cli timed out; dropping the utterance")
                return ""
            if done.returncode != 0:
                logger.error(
                    "whisper-cli exited %s: %s",
                    done.returncode,
                    (done.stderr or "").strip()[-400:],
                )
                return ""
            return _clean(done.stdout)
        finally:
            try:
                os.unlink(wav_path)
            except OSError:
                pass


def _threads() -> int:
    """Threads for whisper.cpp — most of the machine, never all of it.

    Dictation runs while the user is working; taking every core makes the
    desktop stutter for the second it transcribes.
    """
    cpus = os.cpu_count() or 4
    return max(1, min(8, cpus - 2))


def _timeout_for(audio: "np.ndarray", sample_rate: int) -> float:
    """A generous ceiling: a wedged subprocess must not wedge dictation."""
    seconds = len(audio) / float(sample_rate or 16000)
    return max(30.0, seconds * 12)


def _write_wav(path: Path, audio: "np.ndarray", sample_rate: int) -> None:
    """Write mono float32 [-1, 1] as 16-bit PCM, which is what whisper.cpp reads."""
    import numpy as np

    clipped = np.clip(np.asarray(audio, dtype=np.float32), -1.0, 1.0)
    pcm = (clipped * 32767.0).astype(np.int16)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate or 16000))
        wav.writeframes(pcm.tobytes())


def _clean(stdout: str) -> str:
    """Join whisper.cpp's segment lines into one utterance.

    With `-nt` each segment is its own line with leading space. Blank lines and
    the bracketed non-speech markers whisper emits on silence ("[BLANK_AUDIO]",
    "(wind blowing)") are dropped: the engine's own hallucination filter cannot
    see them as speech, and typing them into the user's editor is worse than
    typing nothing.
    """
    lines = []
    for raw in stdout.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            continue
        if line.startswith("(") and line.endswith(")"):
            continue
        lines.append(line)
    return " ".join(lines).strip()
