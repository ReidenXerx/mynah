"""WebRTC VAD wrapper for utterance segmentation.

A thin wrapper around the ``webrtcvad`` package (Google's Voice Activity
Detection). It classifies short PCM frames as speech or silence, which
the engine uses to:
- detect when the user starts speaking (session becomes "active")
- detect when an utterance ends (silence after speech → transcribe)

webrtcvad requires 16 kHz mono 16-bit PCM, in frames of 10, 20, or 30 ms.
The engine delivers audio at 16 kHz, so we chunk into 30 ms frames here.
"""

from __future__ import annotations

import logging

logger = logging.getLogger(__name__)

# webrtcvad accepts frame durations of 10, 20, or 30 ms. 30 ms gives the
# most stable speech/silence boundary detection for dictation.
_FRAME_MS = 30
# At 16 kHz, 30 ms = 480 samples × 2 bytes (16-bit) = 960 bytes per frame.
_SAMPLE_RATE = 16000
_FRAME_BYTES = int(_SAMPLE_RATE * _FRAME_MS / 1000) * 2  # 960


class VoiceActivityDetector:
    """Wraps webrtcvad.Vad with a simple is_speech(frame) -> bool API."""

    def __init__(self, aggressiveness: int = 3, sample_rate: int = _SAMPLE_RATE) -> None:
        """``aggressiveness`` is 0–3 (3 = most aggressive at filtering non-speech)."""
        self._sample_rate = sample_rate
        self._frame_bytes = int(sample_rate * _FRAME_MS / 1000) * 2
        self._vad = None
        try:
            import webrtcvad

            self._vad = webrtcvad.Vad(aggressiveness)
        except ImportError:
            logger.warning(
                "webrtcvad not installed — VAD disabled (dictation will not "
                "segment utterances). Install: pipx inject whiz 'whiz[dictate]'"
            )

    @property
    def available(self) -> bool:
        """True if webrtcvad is loaded and VAD is functional."""
        return self._vad is not None

    @property
    def frame_bytes(self) -> int:
        """The byte length of a single VAD frame (for 30 ms @ 16 kHz = 960)."""
        return self._frame_bytes

    @property
    def frame_duration_ms(self) -> int:
        """The VAD frame duration in milliseconds (30)."""
        return _FRAME_MS

    def is_speech(self, frame: bytes) -> bool:
        """Classify a PCM frame as speech (True) or silence (False).

        ``frame`` must be exactly ``frame_bytes`` bytes of 16-bit mono PCM
        at the configured sample rate. If VAD is unavailable, returns True
        (assume speech — no segmentation, transcribe everything).
        """
        if self._vad is None:
            return True  # no VAD → treat all audio as speech
        if len(frame) != self._frame_bytes:
            # Wrong frame size — webrtcvad would raise. Treat as silence
            # (a partial frame at the end of an utterance is likely quiet).
            return False
        try:
            return self._vad.is_speech(frame, self._sample_rate)
        except Exception:  # noqa: BLE001
            logger.debug("VAD error on frame", exc_info=True)
            return True  # fail open: transcribe rather than drop audio


def frame_bytes_for(sample_rate: int = _SAMPLE_RATE, frame_ms: int = _FRAME_MS) -> int:
    """Compute the byte length of a VAD frame for a given sample rate + duration."""
    return int(sample_rate * frame_ms / 1000) * 2