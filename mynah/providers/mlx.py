"""MlxWhisperProvider — mlx-whisper STT on Apple Silicon (Metal GPU).

mlx-whisper is Apple's MLX-framework Whisper port. Unlike faster-whisper
(which is CPU-only on Mac because CTranslate2 has no Metal backend),
mlx-whisper runs inference on the Apple Silicon GPU via MLX — roughly 7×
faster for the same model on M-series chips.

The model (default ``mlx-community/whisper-large-v3-turbo-mlx-4bit``) is
auto-downloaded from HuggingFace on first use and cached under
``~/.cache/huggingface``. This is a separate format (MLX weights) from
whiz's batch-mode ggml ``.bin`` models, so dictate maintains its own
cache — no sharing with ``whiz transcribe``.

mlx_whisper.transcribe uses a module-level ``ModelHolder`` singleton that
caches the loaded model. ``unload()`` nulls those class variables so the
model can be garbage-collected, dropping ~2 GB of RAM at idle.
"""

from __future__ import annotations

import logging
from typing import TYPE_CHECKING

from whiz.dictate.providers.base import STTProvider

if TYPE_CHECKING:  # pragma: no cover - typing only
    import numpy as np

logger = logging.getLogger(__name__)

# Default model: large-v3-turbo, 4-bit quantized. On Metal, turbo is faster
# than medium AND near-large accuracy — ideal for Russian jargon/obscenity.
# 4-bit quant keeps RAM ~2 GB, fitting 16 GB M1 Pro with headroom.
DEFAULT_MODEL = "mlx-community/whisper-large-v3-turbo-mlx-4bit"

# Whisper's native sample rate. The engine must deliver audio at this rate.
WHISPER_SAMPLE_RATE = 16000


class MlxWhisperProvider(STTProvider):
    """Speech-to-text via mlx-whisper (Apple MLX, Metal GPU)."""

    def __init__(self, model: str = DEFAULT_MODEL) -> None:
        self._model_ref = model
        self._loaded = False

    @property
    def is_loaded(self) -> bool:
        return self._loaded

    def load(self) -> None:
        """Load the model into memory (cold start — may take seconds)."""
        if self._loaded:
            return
        import mlx_whisper  # lazy: keeps `whiz dictate --list-providers` light

        # Touch the model so it downloads + loads now. transcribe() uses
        # ModelHolder.get_model() internally, so we force a load by calling
        # transcribe on a tiny silent buffer. This also surfaces download
        # errors before the user starts speaking.
        import numpy as np

        logger.info("Loading mlx-whisper model: %s", self._model_ref)
        # A 1-second silent float32 buffer triggers model load without
        # producing meaningful output.
        silent = np.zeros(WHISPER_SAMPLE_RATE, dtype=np.float32)
        mlx_whisper.transcribe(
            silent,
            path_or_hf_repo=self._model_ref,
            language="ru",
            verbose=None,
        )
        self._loaded = True
        logger.info("mlx-whisper model loaded")

    def transcribe(
        self,
        audio: np.ndarray,
        sample_rate: int,
        language: str,
        initial_prompt: str,
    ) -> str:
        """Transcribe a mono float32 audio array, returning recognized text.

        ``audio`` must be mono float32 at ``sample_rate`` Hz; if it's not
        16 kHz the engine is responsible for resampling before calling.
        """
        if not self._loaded:
            self.load()
        import mlx_whisper

        # mlx-whisper expects 16 kHz audio. The engine delivers at this rate,
        # but guard against a mismatch to avoid silent garbage output.
        if sample_rate != WHISPER_SAMPLE_RATE:
            raise ValueError(
                f"mlx-whisper requires {WHISPER_SAMPLE_RATE} Hz audio, "
                f"got {sample_rate} Hz"
            )

        result = mlx_whisper.transcribe(
            audio,
            path_or_hf_repo=self._model_ref,
            language=language or "ru",
            initial_prompt=initial_prompt or None,
            condition_on_previous_text=True,
            verbose=None,  # suppress mlx-whisper's tqdm/segment prints
        )
        text = (result.get("text") or "").strip()
        return text

    def unload(self) -> None:
        """Release the model from memory (free RAM at idle).

        mlx_whisper caches the model in a module-level ``ModelHolder``
        singleton. Nulling its class variables lets the GC reclaim the
        ~2 GB of MLX weights.
        """
        if not self._loaded:
            return
        try:
            import mlx_whisper
            from mlx_whisper.transcribe import ModelHolder

            ModelHolder.model = None
            ModelHolder.model_path = None
        except Exception:  # noqa: BLE001 - best-effort cleanup
            logger.debug("unload: could not clear ModelHolder", exc_info=True)
        self._loaded = False
        logger.info("mlx-whisper model unloaded")