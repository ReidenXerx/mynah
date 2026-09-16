"""The dictation engine — platform-agnostic session orchestration.

This is the core of ``mynah``. It ties together:
- a global hotkey listener (pynput) with toggle semantics
- mic capture (sounddevice) at 16 kHz mono
- WebRTC VAD (vad.py) for utterance segmentation
- an STT provider (e.g. MlxWhisperProvider) — spawn-on-demand + idle timeout
- a text injector (e.g. MacTextInjector) — types into the focused app
- a dictation indicator (e.g. MacIndicator) — the floating overlay

Session lifecycle (toggle or push-to-talk):
1. User activates the trigger → session starts: indicator shows ("listening"),
   mic stream opens, STT provider loads (cold start on first session).
   - Toggle mode: press the hotkey to start; press again to stop.
   - Push-to-talk (PTT): hold the hotkey to dictate; release to stop.
2. Audio is captured continuously. VAD detects speech/silence boundaries.
3. When VAD detects end-of-utterance (silence after speech), the buffered
   speech audio is enqueued for transcription. A worker thread transcribes
   it and injects the text into the focused app. The indicator briefly
   shows "transcribing" then returns to "listening".
4. The trigger deactivates (toggle press or PTT release), OR
   ``auto_stop_silence`` seconds of continuous silence elapse → session
   ends: indicator hides, mic stops.
5. After the session ends, the STT provider stays loaded for
   ``idle_timeout`` seconds (default 45) so back-to-back dictation is
   warm, then unloads → zero RAM at idle.

Threading:
- The hotkey listener runs its own thread (pynput).
- The audio capture callback runs on sounddevice's audio thread — it only
  buffers frames, runs VAD, and enqueues completed utterances. It never
  blocks on transcription (which would cause audio dropouts).
- A dedicated worker thread drains the utterance queue: transcribe → inject.
- The indicator (on macOS) must be driven from the main thread's AppKit
  event loop. ``run_dictate()`` starts the NSApplication run loop on the
  main thread and drives the engine from there.
- On non-macOS (or --no-indicator), a simpler ``while`` loop + threading
  drives the engine without AppKit.
"""

from __future__ import annotations

import logging
import os
import math
import queue
import sys
import threading
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING, Callable

from mynah.providers import (
    DictationIndicator,
    STTProvider,
    TextInjector,
    select_indicator,
    select_injector,
    select_stt_provider,
)
from mynah.providers.mlx import WHISPER_SAMPLE_RATE
from mynah.vad import VoiceActivityDetector

if TYPE_CHECKING:  # pragma: no cover - typing only
    from mynah.config import Config

logger = logging.getLogger(__name__)

# Default ``initial_prompt`` when the user hasn't set ``prompt``.
# A Russian sentence in informal/jargon register that biases Whisper away
# from self-censoring obscenity/slang. The model treats the prompt as prior
# context, so seeing informal Russian makes it more likely to reproduce
# informal Russian verbatim instead of sanitizing it.
DEFAULT_RUSSIAN_PROMPT = (
    "Это разговорная запись с неформальной лексикой, сленгом и матом. "
    "Запиши всё как есть, без цензуры: пиздец, охуенно, хуйня, ебать, "
    "заебись, бля, сука, хуй, пизда, мудак."
)

# How long (seconds) of continuous silence ends an utterance within a
# session. Shorter = snappier transcription but may cut off slow speech;
# longer = more natural but adds latency before text appears.
_UTTERANCE_SILENCE = 0.8

# How often (seconds) the run loops poll the stop event.
_TICK = 0.05

# NOTE: the static energy floors (per-frame RMS, whole-utterance RMS,
# minimum utterance length) live in DictateSettings, resolved from
# mynah/config.py (frame_energy / min_energy /
# min_utterance) and pinned against tuning/tuning.toml. There are
# no module-level copies; the adaptive calibration raises them at session
# start via _effective_frame_energy / _effective_min_energy.

# Adaptive noise floor — measure ambient noise at session start (first
# ~1.0s of audio) and raise the energy gates proportionally. The static
# floors in DictateSettings are tuned for a quiet room; in a noisy environment
# (fan, HVAC, MacBook cooler, open window) steady background noise exceeds
# them and webrtcvad misclassifies it as speech, seeding hallucination-prone
# utterances. The adaptive floor uses the median RMS of the calibration
# window as the noise baseline and sets:
#   frame gate      = max(s.frame_energy, noise_floor * _NOISE_FRAME_MULT)
#   utterance gate  = max(s.min_energy,   noise_floor * _NOISE_UTT_MULT)
# (the session settings' static floors — s.frame_energy / s.min_energy).
# The static thresholds remain as floors — a quiet room keeps them, a
# noisy room gets higher gates.
_NOISE_CALIBRATION_SECONDS = 1.0  # sample ambient noise for this long
_NOISE_FRAME_MULT = 3.5           # frame gate ~11dB above noise floor
_NOISE_UTT_MULT = 3.0             # utterance gate ~10dB above noise floor
_NOISE_MIN_SAMPLES = 5            # need ≥this many QUIET frames to trust calibration

# Speech-aware calibration: a calibration frame with RMS at or above this
# level is speech, not noise, and is EXCLUDED from the median. If fewer
# than _NOISE_MIN_SAMPLES quiet frames remain in the window, calibration
# aborts to the static gates for the session. The median alone was assumed
# robust against "brief" speech in the window — but when the user presses
# the hotkey and talks immediately, speech fills the window, the median is
# measured on speech, and the utterance gate rises to ~3x the speech RMS,
# silently dropping the first word. Pinned by the golden corpus's
# speech_during_calibration case (the FIX, not the defect).
# 0.03 ≈ -30dB — the legacy measured per-frame floor; between real
# MacBook-cooler noise (~0.02) and quiet speech (~0.04).
_CALIBRATION_SPEECH_FLOOR = 0.03

# Known Whisper hallucination phrases (lowercased). When the model is fed
# silence/noise it emits these training-data artifacts; we suppress them as
# a safety net even if the energy gate misses (e.g. low-but-audible fan
# noise that VAD misclassifies as speech). Two match modes over the
# trimmed, lowercased transcript (NS-6):
#
# _HALLUCINATION_ARTIFACTS — distinctive boilerplate. Matched by SUBSTRING:
# these strings are not plausible real dictation anywhere in an utterance.
#
# _HALLUCINATION_VOCAB — ordinary single words that ARE plausible real
# dictation ("Отправь перевод на карту"). Matched only when the whole
# transcript EQUALS the phrase — a hallucination IS the entire output.
# Substring matching here silently dropped real speech; the wave-1 audit
# recorded it as a CRITICAL.
_HALLUCINATION_ARTIFACTS = frozenset(
    p.lower()
    for p in (
        "спасибо за субтитры",
        "субтитры создавал",
        "субтитры выполнил",
        "субтитры делал",
        "субтитры подготовил",
        "редактор субтитров",
        "продолжение следует",
        "спасибо за просмотр",
        "спасибо за внимание",
        "подписывайтесь на канал",
        "by follows",
        "by following",
        "amara.org",
        "расскажите о себе",
        # Additional artifacts observed on MacBook cooler/fan noise:
        "следите за обновлениями",
        "оставайтесь с нами",
        "не забудьте подписаться",
        "вы можете поддержать",
    )
)
_HALLUCINATION_VOCAB = frozenset(
    p.lower()
    for p in (
        # Ordinary vocabulary — whole-utterance equality only.
        "субтитры",
        "перевод",
        "корректор",
    )
)

# Sentinel enqueued to tell the transcribe worker to exit its loop.
_FLUSH_SENTINEL = object()

# How much audio one transcription may merge, in bytes of 16-bit mono PCM.
# Whisper's encoder works on a 30-second window; past that it chunks
# internally and the merge stops paying for itself. 20 seconds leaves room
# for the window without risking a split mid-sentence.
_MERGE_LIMIT_SECONDS = 20
_MERGE_LIMIT_BYTES = 2 * 16000 * _MERGE_LIMIT_SECONDS


# The band edges the level meter is drawn from, in hertz. Log-spaced across
# the range speech actually occupies: below 80 Hz is room rumble and a desk
# being knocked, above 5 kHz there is almost nothing left of a voice. Twelve
# bands is what a small meter can show without turning into noise.
_SPECTRUM_BANDS = 12
_SPECTRUM_LOW_HZ = 80.0
_SPECTRUM_HIGH_HZ = 5000.0
# Divides the band magnitudes so a normal speaking voice lands near the top of
# the meter. Measured rather than guessed: across the speech frames of a test
# recording the band magnitudes run 0.001 to 0.05 with a 75th percentile of
# 0.026, so this puts an ordinary syllable around three quarters of full scale
# and leaves headroom for a shout.
_SPECTRUM_REFERENCE = 0.03


def _band_edges(np, frame_samples: int, sample_rate: int) -> list[tuple[int, int]]:
    """Bin ranges for each band, low to high. Computed once per session."""
    import math

    edges = []
    for i in range(_SPECTRUM_BANDS + 1):
        fraction = i / _SPECTRUM_BANDS
        hz = _SPECTRUM_LOW_HZ * math.pow(_SPECTRUM_HIGH_HZ / _SPECTRUM_LOW_HZ, fraction)
        edges.append(int(hz * frame_samples / sample_rate))
    out = []
    for lo, hi in zip(edges, edges[1:]):
        # Every band must own at least one bin, or the low end is empty.
        out.append((lo, max(hi, lo + 1)))
    return out


def _spectrum(mono, np, window, edges) -> list[float]:
    """Band energies for one frame, low to high, each roughly in [0, 1].

    A 480-sample real FFT, which is microseconds — this runs on the audio
    thread, and anything expensive there is heard as a dropout rather than
    seen as a slow meter.

    The transform is normalised by the window's own sum, so a band's value is
    in the signal's amplitude units rather than in unscaled FFT magnitudes.
    Without that every band saturates at 1 and the meter is a solid block —
    which is exactly what the first version drew.
    """
    spectrum = np.abs(np.fft.rfft(mono * window)) / (window.sum() / 2)
    bands = []
    for lo, hi in edges:
        value = float(spectrum[lo:hi].mean()) if hi > lo else 0.0
        # Square root rather than raw magnitude: quiet consonants are most of
        # what makes a meter look alive, and linear scaling hides them.
        bands.append(min(1.0, math.sqrt(value / _SPECTRUM_REFERENCE) if value > 0 else 0.0))
    return bands


def _rms_int16(pcm_bytes: bytes) -> float:
    """RMS amplitude of 16-bit PCM, normalized to 0.0–1.0.

    Uses the stdlib ``array`` module (not numpy) so it works identically in
    the test environment where numpy is faked out.
    """
    import array

    raw = array.array("h")
    raw.frombytes(pcm_bytes)
    if not len(raw):
        return 0.0
    total = sum(v * v for v in raw)
    return (total / len(raw)) ** 0.5 / 32768.0


# How often (seconds) to re-check Accessibility while waiting for the
# user to grant it in System Settings.
_PERM_POLL_INTERVAL = 1.0
# How long (seconds) to wait for the user to grant Accessibility before
# giving up and exiting. Generous — the user may need to navigate System
# Settings, find the binary, and toggle it on.
_PERM_POLL_TIMEOUT = 300  # 5 minutes


def _wait_for_accessibility(injector: TextInjector) -> bool:
    """Poll the Accessibility permission until granted or timeout.

    ``check_permissions(prompt=True)`` was already called, which opened
    System Settings to the Accessibility pane. Now we poll silently (with
    ``prompt=False``) until the user toggles the app on, then return True.
    If the user doesn't grant it within the timeout, return False so the
    caller can exit cleanly.
    """
    deadline = time.monotonic() + _PERM_POLL_TIMEOUT
    while time.monotonic() < deadline:
        time.sleep(_PERM_POLL_INTERVAL)
        ok, _ = injector.check_permissions(prompt=False)
        if ok:
            return True
    return False


@dataclass
class DictateSettings:
    """Resolved dictation settings (config + CLI overrides)."""

    language: str
    initial_prompt: str
    idle_timeout: float
    auto_stop_silence: float
    hotkey: str
    trigger: str
    vad_enabled: bool
    show_indicator: bool
    idle_visible: bool = True
    model: str = ""
    menu_bar: bool = True
    # Energy floors and minimum utterance length.
    #
    # `mynah/config.py` is the single source of truth for these defaults, and
    # `resolve_settings` always passes them explicitly — so these values are
    # only reached by code constructing DictateSettings directly, such as test
    # helpers. They previously mirrored the legacy module constants
    # (0.03/0.025/0.35), which are the too-high thresholds the config keys were
    # added to get away from; a direct construction silently got the old
    # behaviour back. Kept in step with config.py instead.
    frame_energy: float = 0.010
    min_energy: float = 0.008
    min_utterance: float = 0.25


def resolve_settings(config: Config, **overrides: object) -> DictateSettings:
    """Merge config values with CLI overrides into a DictateSettings."""
    prompt = (overrides.get("prompt") or config.prompt or "").strip()
    if not prompt:
        prompt = DEFAULT_RUSSIAN_PROMPT
    return DictateSettings(
        language=(overrides.get("language") or config.language or "ru"),
        initial_prompt=prompt,
        # 0 is a meaningful value ("never unload") — use the sentinel-default
        # pattern like auto_stop_silence below, not an or-chain, or a
        # configured/overridden 0 can never win (W2-M10).
        idle_timeout=float(overrides.get("idle_timeout", config.idle_timeout)),
        auto_stop_silence=float(
            overrides.get("auto_stop_silence", config.auto_stop_silence)
        ),
        hotkey=(overrides.get("hotkey") or config.hotkey or "<cmd>+<shift>+."),
        trigger=(overrides.get("trigger") or config.trigger or "toggle").strip().lower(),
        vad_enabled=bool(overrides.get("vad", config.vad)),
        show_indicator=bool(overrides.get("show_indicator", config.show_indicator)),
        idle_visible=bool(overrides.get("idle_visible", config.idle_visible)),
        model=(overrides.get("model") or config.model or ""),
        menu_bar=bool(overrides.get("menu_bar", config.menu_bar)),
        frame_energy=float(config.frame_energy),
        min_energy=float(config.min_energy),
        min_utterance=float(config.min_utterance),
    )


class DictationEngine:
    """Orchestrates the dictation session lifecycle.

    The engine is constructed with resolved settings + selected providers,
    then driven by ``run()`` which blocks until the user quits (Ctrl+C or
    a second hotkey press that ends the final session).
    """

    def __init__(
        self,
        settings: DictateSettings,
        stt: STTProvider,
        injector: TextInjector,
        indicator: DictationIndicator,
    ) -> None:
        self.s = settings
        self.stt = stt
        self.injector = injector
        self.indicator = indicator

        # Session state — guarded by _state_lock.
        self._state_lock = threading.Lock()
        self._session_active = False
        self._capturing = False
        # PCM frames for the current in-progress utterance — accessed from
        # the audio callback thread only (no lock needed: single writer).
        self._utterance_buffer: list[bytes] = []
        self._in_speech = False
        self._silence_frames = 0
        self._continuous_silence = 0.0  # seconds of silence since last speech
        # Completed utterances ready for transcription — thread-safe queue
        # decoupling the audio callback (fast) from STT (slow).
        self._utterance_queue: queue.Queue = queue.Queue()
        # Whether this session has typed anything yet — see _spaced().
        self._typed_in_session = False
        # Built per session, and only when the indicator draws a spectrum.
        self._spectrum_window = None
        self._spectrum_edges: list[tuple[int, int]] = []
        self._stop_event = threading.Event()
        # Auto-stop request: set by the audio callback when silence elapses,
        # acted on by the run loop OFF the audio thread (the callback must
        # not call _end_session, which joins threads and can block).
        self._end_session_requested = False
        # Session generation counter (W2-H1): incremented each time
        # _start_session actually launches a session's threads. _end_session
        # snapshots it and refuses to mutate shared session state when a
        # newer session started mid-teardown — the zombie-session race
        # where a stale end poisoned the NEW session's queue/worker.
        self._session_generation = 0
        # Idle timeout: after a session ends, unload the model after this long.
        self._idle_timer: threading.Timer | None = None
        # Background threads.
        self._capture_thread: threading.Thread | None = None
        self._transcribe_thread: threading.Thread | None = None
        # VAD
        self._vad = VoiceActivityDetector() if settings.vad_enabled else None
        # Adaptive noise floor: set during calibration at session start.
        # Effective energy gates — raised above the static floors when the
        # ambient noise level warrants it (noisy room). These are the values
        # the capture callback and transcribe worker actually use.
        self._effective_frame_energy = self.s.frame_energy
        self._effective_min_energy = self.s.min_energy
        # Calibration state: collect per-frame RMS during the first
        # _NOISE_CALIBRATION_SECONDS of audio, then compute the noise floor.
        self._noise_cal_rms: list[float] = []
        self._noise_calibrated = False
        # State listeners (e.g. the menu bar) notified on each state change.
        self._indicator_state: str = "idle"
        self._state_listeners: list[Callable[[str], None]] = []
        # Optional menu bar controller; kept here so it isn't GC'd while the
        # AppKit run loop owns the only other reference.
        self._menu_bar: Any = None

    # ---------- public API ----------

    def add_state_listener(self, fn: Callable[[str], None]) -> None:
        """Register a callback notified on every indicator state change.

        Listeners (e.g. the menu bar) receive the new state string
        (``"listening"``/``"transcribing"``/``"idle"``) after the indicator has
        been notified. Called inline from ``_set_state`` on whatever thread
        initiated the state change, so listeners must be thread-safe.
        """
        self._state_listeners.append(fn)

    def _set_state(self, state: str) -> None:
        """Update the indicator state and notify registered listeners.

        Centralizes the ~7 direct ``self.indicator.set_state(...)`` call sites
        so a single place fans the state down to the indicator AND any
        listeners (menu bar, future HUD surfaces). Behavior is identical to
        the direct call when there are no listeners.
        """
        self._indicator_state = state
        self.indicator.set_state(state)
        for fn in self._state_listeners:
            try:
                fn(state)
            except Exception:  # noqa: BLE001
                logger.debug("state listener raised", exc_info=True)

    def run(self) -> int:
        """Start the engine and block until the user quits.

        Returns an exit code (0 = clean).
        """
        # Check permissions before starting. If Accessibility is not yet
        # granted, request it (which opens the System Settings prompt) and
        # poll until the user grants it — like a normal macOS app that waits
        # at the permission dialog rather than crashing and relying on
        # launchd KeepAlive to restart it in a crash loop.
        ok, hint = self.injector.check_permissions()
        if not ok:
            print(hint, file=sys.stderr)
            # Only macOS has a permission the user grants in a dialog while we
            # wait. Elsewhere the check fails because something is missing from
            # the system, and polling for five minutes would just hide the
            # message that says what to install.
            if not _is_macos():
                return 1
            if not _wait_for_accessibility(self.injector):
                return 1
            print("Accessibility granted — starting mynah.", file=sys.stderr)

        # On macOS, run the AppKit event loop on the main thread whenever
        # ANY AppKit UI is live — the indicator OR the menu bar. The menu
        # bar's NSStatusItem needs the run loop to pump events for clicks +
        # menu display; without it (show_indicator=False + menu_bar=True)
        # the status item appears but is dead. Falls back to the plain loop
        # only when no AppKit UI is in use.
        #
        # ALL AppKit UI (indicator panel + menu bar status item) is set up
        # INSIDE _run_with_appkit, AFTER NSApplication.sharedApplication()
        # + setActivationPolicy_(). Creating NSPanel/NSStatusItem before the
        # NSApplication is initialized doesn't render — the window/status
        # item exists in memory but never appears on screen, and image updates
        # (icon tint changes) silently fail. This was the root cause of the
        # pill never showing and the W icon never changing color.
        if (self.s.show_indicator or self.s.menu_bar) and _is_macos():
            return self._run_with_appkit()
        return self._run_plain()

    def stop(self) -> None:
        """Signal the engine to stop (called from a hotkey or Ctrl+C)."""
        self._stop_event.set()
        self._end_session()  # self-locking + idempotent
        if self._idle_timer:
            self._idle_timer.cancel()
            self._idle_timer = None

    def toggle_session(self) -> None:
        """Toggle the dictation session on/off (toggle-mode hotkey callback)."""
        with self._state_lock:
            active = self._session_active
        if active:
            self._end_session()
        else:
            self._start_session()

    # PTT (push-to-talk) callbacks — used when trigger == "ptt".

    def ptt_press(self) -> None:
        """Start dictation when the PTT key is pressed down."""
        with self._state_lock:
            if self._session_active:
                return  # already active — ignore key repeat
        self._start_session()

    def ptt_release(self) -> None:
        """Stop dictation when the PTT key is released."""
        with self._state_lock:
            if not self._session_active:
                return
        self._end_session()

    # ---------- session lifecycle ----------

    def _start_session(self) -> None:
        """Begin a dictation session: load model, show indicator, start capture.

        The model load (cold start, multi-second) happens OUTSIDE the state
        lock so a concurrent stop/PTT-release isn't blocked on it. We mark the
        session active only once the model is ready and the capture/transcribe
        threads are about to start.
        """
        with self._state_lock:
            if self._session_active:
                return  # already active (rapid double-press)
            # Cancel any pending idle unload — we're active again.
            if self._idle_timer:
                self._idle_timer.cancel()
                self._idle_timer = None
            need_load = not self.stt.is_loaded
            # Reserve the session: set active so a concurrent end request waits.
            self._session_active = True

        # Load the STT model (spawn-on-demand). Cold start on first session.
        # Outside the lock: a multi-second download/load must not block the
        # hotkey thread or a concurrent stop().
        if need_load:
            self._set_state("transcribing")
            self.indicator.show()
            try:
                self.stt.load()
            except Exception as e:  # noqa: BLE001
                # A cold-load failure (missing/corrupt model, download
                # error) must not leave the session reserved in
                # "transcribing" with no capture threads running — the
                # user would speak into a dead session with a stuck
                # indicator (H6, wave-1 audit). Recover to idle so the
                # next hotkey press retries cleanly.
                print(
                    f"STT model failed to load: {e}\n"
                    "The session was not started. Check the model setting "
                    "(mynah config) and press the hotkey to retry.",
                    file=sys.stderr,
                )
                logger.warning("Cold STT load failed", exc_info=True)
                with self._state_lock:
                    self._session_active = False
                self._set_state("idle")
                if self.s.idle_visible:
                    self.indicator.show()
                else:
                    self.indicator.hide()
                self._schedule_idle_unload()
                return
        self._set_state("listening")
        self.indicator.show()
        with self._state_lock:
            if not self._session_active:
                # A stop/end raced in while we were loading — abort cleanly.
                self._set_state("idle")
                if self.s.idle_visible:
                    self.indicator.show()
                else:
                    self.indicator.hide()
                self._schedule_idle_unload()
                return
            # Reset utterance state.
            self._utterance_buffer.clear()
            self._in_speech = False
            self._silence_frames = 0
            self._continuous_silence = 0.0
            self._end_session_requested = False
            # Reset adaptive noise calibration for this session — re-measure
            # the ambient noise floor every time a session starts, so a room
            # that got louder/quieter between sessions is handled correctly.
            self._noise_cal_rms = []
            self._noise_calibrated = False
            # Nothing typed yet, so the first utterance is not spaced away
            # from whatever the user was already writing.
            self._typed_in_session = False
            self._effective_frame_energy = self.s.frame_energy
            self._effective_min_energy = self.s.min_energy
            # Claim a new session generation and bind the worker/capture
            # threads to THIS session's objects (W2-H1): the transcribe loop
            # receives the queue object as an argument (never re-reads
            # self._utterance_queue mid-loop) and the capture loop receives
            # the generation, so a teardown racing this start can neither
            # poison nor steal the new session's queue/threads.
            self._session_generation += 1
            generation = self._session_generation
            self._utterance_queue = queue.Queue()
            self._transcribe_thread = threading.Thread(
                target=self._transcribe_loop,
                args=(self._utterance_queue,),
                daemon=True,
            )
            self._transcribe_thread.start()
            # Start capturing audio.
            self._capturing = True
            self._capture_thread = threading.Thread(
                target=self._capture_loop, args=(generation,), daemon=True
            )
            self._capture_thread.start()
        print(
            "▶ Dictation on — speak into the mic. Press the hotkey again to stop.",
            file=sys.stderr,
        )

    def _end_session(self) -> None:
        """End the current session: stop capture, flush remaining, hide indicator.

        Self-locking and idempotent: safe to call from any thread without
        holding ``_state_lock``. Stops the capture stream FIRST and joins the
        capture thread, so the audio callback is no longer mutating
        ``_utterance_buffer`` before we flush it — closing the two-writer race.

        Generation-aware (W2-H1): the session's queue and threads are
        snapshotted under the first lock. If a NEW session starts while this
        teardown is joining threads (user re-arms the hotkey during the
        ≤30s transcribe join, or two menu-bar clicks race), the snapshots
        belong to the OLD session — the flush sentinel goes to the SNAPSHOT
        queue (releasing the old worker) and never to the new session's, and
        none of the new session's state (buffer, thread refs, indicator,
        idle timer) is touched.
        """
        with self._state_lock:
            if not self._session_active:
                return
            self._session_active = False
            self._capturing = False
            generation = self._session_generation
            capture_thread = self._capture_thread
            transcribe_thread = self._transcribe_thread
            utterance_queue = self._utterance_queue

        # Join the capture thread OFF the lock (it polls _capturing every
        # _TICK). Once joined, its sounddevice stream is closed and the audio
        # callback can no longer touch _utterance_buffer. Skip the join when
        # _end_session is called FROM the capture thread itself (auto-stop
        # path) — a thread cannot join itself.
        if (
            capture_thread
            and capture_thread.is_alive()
            and capture_thread is not threading.current_thread()
        ):
            capture_thread.join(timeout=5.0)

        with self._state_lock:
            superseded = self._session_generation != generation
            if not superseded:
                self._capture_thread = None
                # Now safe to flush the buffer — the audio callback is gone.
                if self._utterance_buffer:
                    utterance_queue.put(b"".join(self._utterance_buffer))
                    self._utterance_buffer.clear()
                    self._in_speech = False
                    self._silence_frames = 0
            # Tell the transcribe worker to drain and exit after remaining
            # work. ALWAYS the snapshot queue: a concurrent _start_session
            # has already swapped self._utterance_queue to the new session's,
            # and a sentinel landing THERE would kill the new worker
            # instantly (the W2-H1 zombie).
            utterance_queue.put(_FLUSH_SENTINEL)

        # Wait for the transcribe worker to finish pending utterances (off the
        # lock — this can take a while for long trailing utterances).
        if transcribe_thread and transcribe_thread.is_alive():
            transcribe_thread.join(timeout=30.0)
        with self._state_lock:
            if self._session_generation == generation:
                self._transcribe_thread = None
        # A newer session started during our teardown: it owns the indicator,
        # the idle timer, and the user-facing notices now. Our worker was
        # drained above; leave without touching its state.
        with self._state_lock:
            superseded = self._session_generation != generation
        if superseded:
            return
        self._set_state("idle")
        # When the idle badge is visible, keep the dimmed indicator on screen
        # after a session ends instead of hiding it — so the service always
        # shows an "armed" state. hide() reverts to the original behavior
        # when idle_visible is off.
        if self.s.idle_visible:
            self.indicator.show()
        else:
            self.indicator.hide()
        # Schedule idle unload.
        self._schedule_idle_unload()
        print("■ Dictation off.", file=sys.stderr)

    def _schedule_idle_unload(self) -> None:
        """After idle_timeout, unload the STT model to free RAM."""
        if self._idle_timer:
            self._idle_timer.cancel()
        if self.s.idle_timeout <= 0:
            return
        self._idle_timer = threading.Timer(self.s.idle_timeout, self._idle_unload)
        self._idle_timer.daemon = True
        self._idle_timer.start()

    def _idle_unload(self) -> None:
        """Unload the STT model (called by the idle timer)."""
        with self._state_lock:
            if self._session_active:
                return  # user restarted dictation
            if self.stt.is_loaded:
                self.stt.unload()
                logger.info(
                    "STT model unloaded after idle timeout (%ss)", self.s.idle_timeout
                )

    # ---------- audio capture + VAD ----------

    def _capture_loop(self, generation: int | None = None) -> None:
        """Background thread: open the mic stream and feed the audio callback.

        The sounddevice callback runs on its own audio thread and must not
        block. It only buffers frames, runs VAD, computes the indicator
        level, and enqueues completed utterances. Transcription happens on
        the separate ``_transcribe_loop`` thread.

        ``generation`` binds this loop to the session that started it
        (W2-H1): once a newer session starts, this loop and its audio
        callback stop touching shared state instead of feeding the new
        session's buffer/queue. ``None`` binds to the current generation
        (direct/test calls).
        """
        if generation is None:
            generation = self._session_generation
        try:
            import sounddevice as sd
        except ImportError:
            print(
                "sounddevice not installed. Install the runtime extra:\n"
                "  pipx inject mynah 'mynah[macos]'",
                file=sys.stderr,
            )
            self._stop_event.set()
            return

        vad = self._vad
        frame_bytes = vad.frame_bytes if vad and vad.available else 960
        frame_samples = frame_bytes // 2
        frame_seconds = frame_samples / WHISPER_SAMPLE_RATE

        # The spectrum's window and band edges depend only on the frame size,
        # so they are built here rather than 33 times a second.
        if getattr(self.indicator, "wants_spectrum", False):
            import numpy as _np

            self._spectrum_window = _np.hanning(frame_samples)
            self._spectrum_edges = _band_edges(_np, frame_samples, WHISPER_SAMPLE_RATE)
        else:
            self._spectrum_window = None
            self._spectrum_edges = []

        def callback(indata, frames, time_info, status):  # noqa: ARG001
            """sounddevice stream callback — runs on the audio thread."""
            # Generation guard (W2-H1): a stale stream (its join timed out)
            # must not feed a newer session's buffer/queue.
            if not self._capturing or self._session_generation != generation:
                return
            import numpy as np

            mono = indata[:, 0]
            # 16-bit PCM bytes for VAD.
            pcm = (mono * 32767).astype(np.int16).tobytes()
            # RMS amplitude for the indicator volume curve (0.0–1.0).
            rms = float(np.sqrt(np.mean(mono ** 2)))
            level = min(1.0, rms * 5.0)
            if self._spectrum_window is not None:
                # Only when something is going to draw it.
                self.indicator.update_spectrum(
                    _spectrum(mono, np, self._spectrum_window, self._spectrum_edges)
                )
            self.indicator.update_level(level)
            # Adaptive noise floor calibration: during the first
            # _NOISE_CALIBRATION_SECONDS of audio, collect per-frame RMS.
            # Once enough frames are collected, compute the median as the
            # ambient noise baseline and raise the energy gates so steady
            # background noise (fan, HVAC) doesn't pass as speech.
            if not self._noise_calibrated:
                self._noise_cal_rms.append(rms)
                cal_frames_needed = int(
                    _NOISE_CALIBRATION_SECONDS / frame_seconds
                ) + 1
                if len(self._noise_cal_rms) >= max(cal_frames_needed, _NOISE_MIN_SAMPLES):
                    self._finish_noise_calibration()
            # VAD segmentation (or no-VAD passthrough).
            if vad and vad.available:
                self._process_vad_frames(pcm, frame_bytes, frame_seconds)
            else:
                self._utterance_buffer.append(pcm)

        try:
            with sd.InputStream(
                samplerate=WHISPER_SAMPLE_RATE,
                channels=1,
                dtype="float32",
                blocksize=frame_samples,
                callback=callback,
            ):
                while (
                    self._capturing
                    and not self._stop_event.is_set()
                    and self._session_generation == generation
                ):
                    sd.sleep(int(_TICK * 1000))
        except Exception as e:  # noqa: BLE001
            print(f"Microphone error: {e}", file=sys.stderr)
            msg = str(e).lower()
            if "input" in msg or "device" in msg:
                print(
                    "Microphone access denied or no input device. Grant Microphone "
                    "permission in System Settings → Privacy & Security → Microphone.",
                    file=sys.stderr,
                )
            self._stop_event.set()
            # Clean up the session (hide indicator, flush) off the audio
            # thread — unless a newer session already started (W2-H1): a
            # stale stream erroring late must not end IT.
            if self._session_generation == generation:
                self._end_session()
            return
        # The stream closed (hotkey toggle/PTT release stopped us) OR auto-stop
        # set the flag. Auto-stop must end the session here — off the audio thread,
        # so we don't block the sounddevice callback on a thread join. The flag
        # belongs to THIS session's generation only.
        if (
            self._end_session_requested
            and not self._stop_event.is_set()
            and self._session_generation == generation
        ):
            self._end_session_requested = False
            self._end_session()

    def _finish_noise_calibration(self) -> None:
        """Compute the ambient noise floor from collected calibration RMS values.

        Speech-aware: frames with RMS at or above _CALIBRATION_SPEECH_FLOOR
        are speech, not noise, and are excluded from the median — speech in
        the calibration window (user talks immediately) must not poison the
        noise floor, which previously raised the utterance gate above the
        speech RMS and silently dropped the first word. If fewer than
        _NOISE_MIN_SAMPLES quiet frames remain, there is nothing to
        measure noise from: calibration completes leaving the static gates
        in force for the session.

        The raised gates cap the calibration contribution at
        _CALIBRATION_SPEECH_FLOOR (M13, wave-2) — the speech/noise
        discrimination line: a calibrated gate above it would demand
        speech louder than speech, and no frame_energy/min_energy setting
        could ever take effect against it.

        Called once from the audio callback after enough frames are collected.
        Safe to call multiple times — _noise_calibrated guards re-entry.
        """
        if self._noise_calibrated:
            return
        self._noise_calibrated = True
        samples = self._noise_cal_rms
        # Free the calibration buffer regardless of the path taken.
        self._noise_cal_rms = []
        quiet = [r for r in samples if r < _CALIBRATION_SPEECH_FLOOR]
        if len(quiet) < _NOISE_MIN_SAMPLES:
            # Window dominated by speech (or too short): keep static gates.
            logger.info(
                "Noise calibration aborted: %d/%d quiet frames; static gates stay in force",
                len(quiet), len(samples),
            )
            return
        sorted_rms = sorted(quiet)
        n = len(sorted_rms)
        median = sorted_rms[n // 2] if n % 2 else (sorted_rms[n // 2 - 1] + sorted_rms[n // 2]) / 2
        # Raise the effective gates above the noise floor. The static floors
        # remain as minimums — a quiet room keeps them — and the CALIBRATION
        # CONTRIBUTION is capped at _CALIBRATION_SPEECH_FLOOR (M13, wave-2):
        # the floor is the speech/noise discrimination line, so a gate above
        # it would demand speech LOUDER than speech — a measured fan median
        # of 0.02 put the frame gate at 0.07 and silently discarded normal
        # talking (~0.05-0.06 RMS), and lowering frame_energy changed nothing
        # because max() kept the calibrated floor dominant. Capping the
        # median's contribution (not the whole max) keeps the static floors
        # as true minimums: a user floor above the cap applies exactly as
        # set. The cap cannot misfire — the median is measured only on
        # quiet frames (below the floor), so the contribution only ever
        # rises toward the cap from below.
        frame_gate = max(
            self.s.frame_energy,
            min(median * _NOISE_FRAME_MULT, _CALIBRATION_SPEECH_FLOOR),
        )
        utt_gate = max(
            self.s.min_energy,
            min(median * _NOISE_UTT_MULT, _CALIBRATION_SPEECH_FLOOR),
        )
        self._effective_frame_energy = frame_gate
        self._effective_min_energy = utt_gate
        logger.info(
            "Adaptive noise floor calibrated: median RMS=%.4f (from %d quiet frames), "
            "frame gate=%.4f, utterance gate=%.4f",
            median, len(quiet), frame_gate, utt_gate,
        )

    def _process_vad_frames(self, pcm: bytes, frame_bytes: int, frame_seconds: float) -> None:
        """Run VAD on PCM frames and manage utterance buffering/enqueuing."""
        vad = self._vad
        offset = 0
        while offset + frame_bytes <= len(pcm):
            chunk = pcm[offset : offset + frame_bytes]
            offset += frame_bytes
            # Energy pre-filter: reject frames below the amplitude floor as
            # silence before VAD — webrtcvad misclassifies steady low-level
            # noise as speech, which seeds hallucination-prone utterances.
            # Use the adaptive effective floor (raised in noisy rooms).
            if _rms_int16(chunk) < self._effective_frame_energy:
                is_speech = False
            else:
                is_speech = vad.is_speech(chunk)
            if is_speech:
                self._utterance_buffer.append(chunk)
                self._in_speech = True
                self._silence_frames = 0
                self._continuous_silence = 0.0
            else:
                # Silence frame.
                if self._in_speech:
                    # Buffer trailing silence (natural pacing for the model).
                    self._utterance_buffer.append(chunk)
                    self._silence_frames += 1
                    # End of utterance: enough silence after speech.
                    if self._silence_frames * frame_seconds >= _UTTERANCE_SILENCE:
                        self._enqueue_utterance()
                # Track continuous silence for auto-stop.
                self._continuous_silence += frame_seconds
                if (
                    self.s.auto_stop_silence > 0
                    and self._continuous_silence >= self.s.auto_stop_silence
                    and not self._in_speech
                ):
                    # Auto-stop: request the session end. Do NOT call
                    # _end_session() here — it joins threads (capture +
                    # transcribe) and would block the audio callback for up
                    # to ~30s. The capture loop sees the flag and ends the
                    # session off the audio thread.
                    self._end_session_requested = True
                    self._capturing = False  # stop the capture loop promptly

    def _enqueue_utterance(self) -> None:
        """Move the current utterance buffer to the transcription queue."""
        if not self._utterance_buffer:
            return
        self._utterance_queue.put(b"".join(self._utterance_buffer))
        self._utterance_buffer.clear()
        self._in_speech = False
        self._silence_frames = 0

    def _spaced(self, text: str) -> str:
        """A separator between one utterance and the next, within a session.

        Whisper returns each utterance trimmed, so typing them as they come
        runs them together — "the fee is computedon gross". A leading space
        fixes that, except before punctuation that belongs to the word before
        it, and except for the first utterance of a session, which would
        otherwise indent whatever the user was already writing.
        """
        if not self._typed_in_session or not text:
            return text
        if text[0] in ",.!?;:)]}»…":
            return text
        return " " + text

    # ---------- transcription worker ----------

    def _transcribe_loop(self, utterance_queue: queue.Queue | None = None) -> None:
        """Worker thread: drain the utterance queue, transcribe, inject.

        Bound to the session's OWN queue object (W2-H1): never re-reads
        ``self._utterance_queue`` mid-loop, so a session started after this
        one can neither steal this worker's input nor have its queue
        poisoned by an old session's flush sentinel.
        """
        import numpy as np

        if utterance_queue is None:
            utterance_queue = self._utterance_queue
        while True:
            item = utterance_queue.get()
            if item is _FLUSH_SENTINEL:
                break
            merged, flushed = self._drain_queued(utterance_queue, item)
            self._transcribe_and_inject(merged, np)
            if flushed:
                break

    def _drain_queued(self, utterance_queue: queue.Queue, first: bytes) -> tuple[bytes, bool]:
        """Join ``first`` with whatever else is already waiting behind it.

        Whisper encodes a 30-second window whatever it is given, so three
        utterances cost three full encodes while one merged utterance costs
        one. That is what makes speaking in short bursts — talk, pause, talk —
        fall further and further behind: every pause bought another encode.

        Merging only ever takes what is ALREADY queued, so nothing waits for
        audio that has not happened yet: with a free worker the first utterance
        is transcribed alone and immediately, and batching appears only when
        the user is talking faster than the machine transcribes, which is
        exactly when it is needed.

        The pieces are adjacent audio from one session, so concatenating them
        also gives the model more context than a two-word fragment has, which
        is what makes the text read as a sentence rather than as scraps.

        Returns the merged PCM and whether the flush sentinel was consumed
        while draining (the session is ending; the caller must stop after this
        last transcription, which is exactly what the sentinel asked for).
        """
        parts = [first]
        total = len(first)
        flushed = False
        while total < _MERGE_LIMIT_BYTES:
            try:
                item = utterance_queue.get_nowait()
            except queue.Empty:
                break
            if item is _FLUSH_SENTINEL:
                flushed = True
                break
            parts.append(item)
            total += len(item)
        if len(parts) > 1:
            logger.debug("Merging %d queued utterances (%.1fs)", len(parts),
                         total / (2 * WHISPER_SAMPLE_RATE))
        return b"".join(parts), flushed

    def _transcribe_and_inject(self, pcm_bytes: bytes, np) -> None:
        """Transcribe a PCM utterance and inject the text into the focused app."""
        if len(pcm_bytes) < 2 * WHISPER_SAMPLE_RATE * self.s.min_utterance:
            # Too short to be meaningful speech — skip.
            return
        # Energy gate: Whisper hallucinates training-data boilerplate on
        # near-silent / noise-only audio. Skip utterances below the RMS floor
        # before spending a transcription on them.
        energy = _rms_int16(pcm_bytes)
        if energy < self._effective_min_energy:
            logger.debug("Skipping low-energy utterance (%.4f < %.4f)", energy, self._effective_min_energy)
            return
        samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32) / 32768.0
        duration = len(samples) / WHISPER_SAMPLE_RATE
        logger.debug("Transcribing utterance: %.2fs, energy=%.4f", duration, energy)
        self._set_state("transcribing")
        try:
            text = self.stt.transcribe(
                samples,
                sample_rate=WHISPER_SAMPLE_RATE,
                language=self.s.language,
                initial_prompt=self.s.initial_prompt,
            )
        except Exception as e:  # noqa: BLE001
            # The utterance is lost either way — but vanishing silently
            # leaves the user speaking into a session that types nothing,
            # with no diagnostic anywhere (W2-M6). Be loud: stderr lands in
            # the LaunchAgent log under launchd and in the terminal
            # otherwise. The worker stays alive for later utterances.
            print(f"Transcription failed, utterance dropped: {e}", file=sys.stderr)
            logger.warning("Transcription failed", exc_info=True)
            self._set_state("listening")
            return
        text_norm = text.strip().lower()
        if text_norm:
            # Hallucination safety net: suppress known Whisper training-data
            # artifacts that slip through when the audio is just above the
            # energy floor (e.g. steady fan/keyboard noise). Two match modes
            # over the trimmed, lowercased transcript (NS-6): distinctive
            # artifact phrases match by SUBSTRING; ordinary vocabulary words
            # ("перевод") match only when they ARE the whole utterance —
            # substring matching there silently dropped real speech
            # ("Отправь перевод на карту"), the wave-1 CRITICAL.
            if (
                any(marker in text_norm for marker in _HALLUCINATION_ARTIFACTS)
                or text_norm in _HALLUCINATION_VOCAB
            ):
                logger.warning("Suppressing hallucination: %r", text)
                self._set_state("listening")
                return
            logger.debug("Injecting text: %s", text)
            try:
                self.injector.type_text(self._spaced(text))
                self._typed_in_session = True
            except Exception as e:  # noqa: BLE001
                # An injection failure (Accessibility revoked, target app
                # gone, ...) must never propagate: this runs on the
                # transcribe worker thread, and an unwrapped raise kills
                # the loop — every later utterance would be transcribed
                # and silently dropped with the pill stuck on
                # "listening" (H2, wave-1 audit). Warn and keep the
                # worker alive.
                logger.warning("Text injection failed: %s", e)
        self._set_state("listening")

    # ---------- run loops ----------

    def _setup_menu_bar(self) -> None:
        """Create the macOS menu bar status item, if enabled and available.

        Uses the rumps-based MacMenuBar provider which handles NSApplication
        lifecycle, NSStatusItem, and NSMenu internally. Degrades to a no-op
        when ``settings.menu_bar`` is off or rumps/pyobjc is unavailable.
        Keeps a strong reference on ``self._menu_bar`` so the status item
        survives for the lifetime of the engine.
        """
        if not self.s.menu_bar or not _is_macos():
            logger.debug("_setup_menu_bar: skipped (menu_bar=%s, macos=%s)", self.s.menu_bar, _is_macos())
            return
        try:
            from mynah.providers.macos_rumps import MacMenuBar
        except ImportError:
            logger.debug("menu bar provider unavailable (rumps/pyobjc?)", exc_info=True)
            return
        try:
            mb = MacMenuBar(engine=self)
            mb.setup()
            self.add_state_listener(mb.on_state)
            self._menu_bar = mb
            logger.debug("_setup_menu_bar: rumps menu bar created and state listener added")
        except Exception:  # noqa: BLE001
            logger.warning("Could not create dictation menu bar item", exc_info=True)

    def _run_plain(self) -> int:
        """Run without AppKit — a blocking loop driving the hotkey listener.

        On the non-AppKit path, the indicator is a NullIndicator (or a
        platform indicator that doesn't need NSApplication), so setup()
        is safe to call here.
        """
        self.indicator.setup()
        if self.s.show_indicator and self.s.idle_visible:
            self._set_state("idle")
            self.indicator.show()
        server = self._start_control_server()
        listener = self._start_hotkey_listener()
        if listener is None and server is None:
            print(
                "Nothing can start a session: no hotkey listener and no control "
                "socket. Dictation will sit idle.",
                file=sys.stderr,
            )
        try:
            while not self._stop_event.is_set():
                time.sleep(_TICK)
        except KeyboardInterrupt:
            self.stop()
        finally:
            if listener:
                listener.stop()
            if server:
                server.stop()
        return 0

    def _start_control_server(self):
        """Serve the control socket, so the compositor and shell can drive us.

        Only on Linux: macOS owns its hotkey and draws its own indicator, so
        nothing outside the process needs to speak to it. Returns the server,
        or None when there is none to run (a socket we could not open is a
        reason to say so, not to refuse to dictate).
        """
        if _is_macos():
            return None
        from mynah import control

        server = control.ControlServer(
            on_toggle=self.toggle_session,
            on_start=self.ptt_press,     # idempotent: start if not running
            on_stop=self.ptt_release,    # idempotent: stop if running
            on_quit=self.stop,
            state=lambda: self._indicator_state,
        )
        try:
            server.start()
        except Exception as e:  # noqa: BLE001
            print(f"Control socket unavailable: {e}", file=sys.stderr)
            return None
        print(
            f"mynah is listening on {server.path}\n"
            f"  Bind a key to:  mynah toggle\n"
            f"  Watch it with:  mynah watch",
            file=sys.stderr,
        )
        return server

    def _run_with_appkit(self) -> int:
        """Run with the macOS AppKit event loop, owned by rumps.

        rumps.App.run() handles NSApplication.sharedApplication(),
        setActivationPolicy, the NSStatusItem, NSMenu, and the full AppKit
        event loop — including window display, status item redraw, and menu
        popup. This replaces the manual NSApplication.run() + custom ObjC glue
        that couldn't properly render the indicator panel or update the
        status item icon.

        The floating pill indicator is created inside a ``rumps.events.
        before_start`` callback, which fires AFTER
        ``NSApplication.sharedApplication()`` has been initialized but BEFORE
        ``AppHelper.runEventLoop()`` blocks. This is the crucial timing: an
        NSPanel created before the NSApplication exists never renders, and
        one created while the run loop is already blocking must be
        dispatched to the main thread. ``before_start`` gives us a main-thread
        callback at the exact safe window, so the panel's vibrancy, waveform,
        and fade all render correctly under rumps' proper event loop — the
        thing the old manual ``NSApplication.run()`` + background watcher
        couldn't provide.
        """
        # NOTE: the import below cannot raise ImportError on rumps —
        # macos_rumps defers `import rumps` inside MacMenuBar.setup(). The
        # reachable rumps-missing signal is `self._menu_bar is None` after
        # _setup_menu_bar() (checked below).
        from mynah.providers.macos_rumps import MacMenuBar  # noqa: F401

        # Create the rumps-based menu bar (sets up NSStatusItem + NSMenu).
        self._setup_menu_bar()
        if (
            self.s.menu_bar
            and self._menu_bar is None
            and os.environ.get("MYNAH_SERVICE") == "1"
        ):
            # Menu bar requested but not created (rumps/pyobjc missing or
            # broken — see the setup warning in the log). Under the
            # LaunchAgent the menu bar is the service's ONLY UI
            # (Start/Stop/Quit); a silent hotkey-only fallback looks healthy
            # in `mynah service status` while KeepAlive relaunches it
            # uselessly. Exit 1 so the LastExitStatus churn is visible
            # (W2-H2). In a terminal the degraded mode is visible and
            # interactive — the plain-loop fallback stays.
            print(
                "rumps/pyobjc unavailable — the dictation service cannot show "
                "its menu bar, so it will not start. Reinstall the dictate "
                "extra, then reinstall the service:\n"
                "  pipx inject mynah 'mynah[macos]' --force\n"
                "  mynah service uninstall && mynah service install",
                file=sys.stderr,
            )
            return 1

        # Set up the floating pill indicator in rumps' before_start event —
        # fires on the main thread after NSApplication.sharedApplication()
        # exists but before the run loop blocks. This is the safe window for
        # NSPanel creation; doing it earlier (before rumps creates the
        # NSApplication) meant the panel never rendered.
        try:
            import rumps.events as rumps_events

            def _setup_indicator(*_args, **_kwargs) -> None:
                self.indicator.setup()
                if self.s.show_indicator and self.s.idle_visible:
                    self._set_state("idle")
                    self.indicator.show()

            rumps_events.before_start.register(_setup_indicator)
        except Exception:  # noqa: BLE001
            logger.debug("could not register before_start indicator setup", exc_info=True)

        # Start the hotkey listener on a background thread.
        listener = self._start_hotkey_listener()

        # Run the rumps App event loop — blocks until quit.
        # rumps handles NSApplication init, accessory policy, the run loop,
        # and termination (via our Quit menu item calling rumps.quit_application).
        try:
            if self._menu_bar is not None:
                self._menu_bar.run()
            else:
                # Menu bar creation failed — fall back to plain loop so the
                # hotkey still works.
                while not self._stop_event.is_set():
                    time.sleep(_TICK)
        except KeyboardInterrupt:
            self.stop()
        finally:
            if listener:
                listener.stop()
        return 0

    def _start_hotkey_listener(self):
        """Start the pynput global hotkey listener. Returns the listener or None.

        In toggle mode, the hotkey is registered via ``GlobalHotKeys``
        (a combo like ``<cmd>+<shift>+d``); pressing it flips the session on/off.
        In PTT mode, we use a low-level ``Listener`` that watches the parsed
        key go down (start) and up (stop), so holding = dictating, release =
        stop. PTT works best with a single key (e.g. ``<f8>``) or a simple
        modifier+key combo; pynput only fires press/release for the final key
        in a combo, which is exactly what we want.
        """
        if not _is_macos():
            # No client can grab a global hotkey on Wayland — that is the
            # point of Wayland. The compositor binds the key and runs
            # `mynah toggle`, which reaches us through the control socket.
            logger.debug("hotkey is the compositor's on this platform")
            return None

        try:
            from pynput import keyboard
        except ImportError:
            print(
                "pynput not installed — cannot listen for the hotkey.\n"
                "Install the macos extra: pipx inject mynah 'mynah[macos]'\n"
                "Or press Ctrl+C to quit (dictation won't toggle without a hotkey).",
                file=sys.stderr,
            )
            return None

        if self.s.trigger == "ptt":
            return self._start_ptt_listener(keyboard)
        return self._start_toggle_listener(keyboard)

    def _start_toggle_listener(self, keyboard):
        """Toggle mode: register the combo and flip the session on each press."""
        try:
            keyboard.HotKey.parse(self.s.hotkey)  # validate early
        except Exception as e:  # noqa: BLE001
            print(f"Invalid hotkey '{self.s.hotkey}': {e}", file=sys.stderr)
            print(
                "The hotkey listener could not start, so dictation cannot be "
                "triggered. Fix the hotkey (e.g. mynah set hotkey=\"<cmd>+<shift>+.\")\n"
                "then restart mynah.",
                file=sys.stderr,
            )
            # Stop the engine instead of running with no way to trigger it —
            # a listenerless agent would look alive but never respond, which
            # is worse than a visible crash (especially under a KeepAlive
            # LaunchAgent that would otherwise silently loop forever).
            self._stop_event.set()
            return None

        def on_activate():
            self.toggle_session()

        listener = keyboard.GlobalHotKeys({self.s.hotkey: on_activate})
        listener.start()
        return listener

    def _start_ptt_listener(self, keyboard):
        """PTT mode: hold the hotkey to dictate; release to stop.

        We track the currently-held modifiers and require the full combo to
        be held down (all modifiers + the final key) before a press counts.
        Without this, a combo like ``<ctrl>+<space>`` would fire on a bare
        Space — hijacking the spacebar.

        PTT uses a low-level ``Listener`` (press/release events) rather
        than ``GlobalHotKeys`` because hold-to-talk needs key-up as
        well as key-down. Works with a single key (e.g. ``<f8>``) or a
        modifier+key combo (e.g. ``<ctrl>+<space>``).
        """
        try:
            parsed = keyboard.HotKey.parse(self.s.hotkey)
        except Exception as e:  # noqa: BLE001
            print(f"Invalid hotkey '{self.s.hotkey}': {e}", file=sys.stderr)
            print(
                "The hotkey listener could not start, so dictation cannot be "
                "triggered. Fix the hotkey (e.g. mynah set hotkey=\"<cmd>+<shift>+.\")\n"
                "then restart mynah.",
                file=sys.stderr,
            )
            self._stop_event.set()
            return None

        if not parsed:
            print(f"Invalid hotkey '{self.s.hotkey}': empty combo", file=sys.stderr)
            self._stop_event.set()
            return None

        target_key = parsed[-1]
        required_modifiers = set(parsed[:-1])

        def _is_modifier(key) -> bool:
            # pynput represents modifiers as Key.* enum values (ctrl_l,
            # shift_r, alt_l, cmd_r, ...). Match by the .name attribute;
            # plain KeyCode (letters/digits) has no such modifier name.
            name = getattr(key, "name", "") or ""
            return name in {
                "ctrl", "ctrl_l", "ctrl_r",
                "alt", "alt_l", "alt_r", "alt_gr",
                "shift", "shift_l", "shift_r",
                "cmd", "cmd_l", "cmd_r",
            }

        held_modifiers: set = set()

        def on_press(key):
            if _is_modifier(key):
                held_modifiers.add(key)
                return
            if key == target_key and required_modifiers <= held_modifiers:
                self.ptt_press()

        def on_release(key):
            if _is_modifier(key):
                held_modifiers.discard(key)
                return
            if key == target_key:
                # Releasing the target key ends dictation even if modifiers
                # are still held (they'll be cleaned up on their own release).
                self.ptt_release()

        listener = keyboard.Listener(on_press=on_press, on_release=on_release)
        listener.start()
        return listener


def _is_macos() -> bool:
    return sys.platform == "darwin"


def run_dictate(config: Config, **overrides: object) -> int:
    """Entry point: select providers, build the engine, and run it.

    Called by ``cli.cmd_dictate``. Returns an exit code.
    """
    settings = resolve_settings(config, **overrides)

    # Select providers for this platform.
    try:
        stt = select_stt_provider(config)
        injector = select_injector(config)
        indicator = select_indicator(config)
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    # The configured model, or the provider's own default when it is empty.
    if settings.model:
        stt.model_ref = settings.model

    engine = DictationEngine(settings, stt, injector, indicator)
    model_name = stt.model_ref or "the provider's default"
    trigger_label = "push-to-talk" if settings.trigger == "ptt" else "toggle"
    print(
        f"mynah — {trigger_label}: {settings.hotkey}  |  model: {model_name}  "
        f"|  language: {settings.language}",
        file=sys.stderr,
    )
    if settings.trigger == "ptt":
        print("Hold the hotkey to dictate; release to stop. Ctrl+C to quit.", file=sys.stderr)
    else:
        print("Press the hotkey to start/stop dictation. Ctrl+C to quit.", file=sys.stderr)
    return engine.run()
