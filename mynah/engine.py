"""The dictation engine — platform-agnostic session orchestration.

This is the core of ``whiz dictate``. It ties together:
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
import queue
import sys
import threading
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING

from whiz.dictate.providers import (
    DictationIndicator,
    STTProvider,
    TextInjector,
    select_indicator,
    select_injector,
    select_stt_provider,
)
from whiz.dictate.providers.mlx import WHISPER_SAMPLE_RATE
from whiz.dictate.vad import VoiceActivityDetector

if TYPE_CHECKING:  # pragma: no cover - typing only
    from whiz.config import Config

logger = logging.getLogger(__name__)

# Default ``initial_prompt`` when the user hasn't set ``dictate_prompt``.
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

# Per-frame energy floor (normalized 0.0–1.0). Frames below this amplitude
# are treated as silence BEFORE VAD even sees them — webrtcvad can classify
# steady low-level noise (fan, keyboard, HVAC) as speech, which produces
# utterances that pass the whole-buffer RMS gate but are still garbage.
# This floor short-circuits those frames so they never start/extend an
# utterance. 0.025 ≈ -32dB — above typical Mac mic room noise, below quiet
# speech.
_VAD_FRAME_ENERGY = 0.025

# How often (seconds) the run loops poll the stop event.
_TICK = 0.05

# Minimum utterance length (seconds) to bother transcribing — sub-0.35s
# blips are noise/clicks/breaths, not speech.
_MIN_UTTERANCE_SECONDS = 0.35

# Minimum RMS energy (0.0–1.0, normalized int16) for an utterance to be
# transcribed. Below this the audio is silence or noise — Whisper is known
# to hallucinate training-data boilerplate ("субтитры создавал…",
# "продолжение следует…") on near-silent input, so we skip it entirely.
# 0.02 ≈ -34dB — above typical Mac mic room noise floor.
_MIN_ENERGY = 0.02

# Known Whisper hallucination phrases (lowercased). When the model is fed
# silence/noise it emits these training-data artifacts; we suppress them as
# a safety net even if the energy gate misses (e.g. low-but-audible fan
# noise that VAD misclassifies as speech).
_HALLUCINATION_PHRASES = frozenset(
    p.lower()
    for p in (
        "спасибо за субтитры",
        "субтитры создавал",
        "субтитры выполнил",
        "субтитры делал",
        "субтитры подготовил",
        "редактор субтитров",
        "корректор",
        "перевод",
        "продолжение следует",
        "спасибо за просмотр",
        "спасибо за внимание",
        "подписывайтесь на канал",
        "by follows",
        "by following",
        "amara.org",
        "расскажите о себе",
    )
)

# Sentinel enqueued to tell the transcribe worker to exit its loop.
_FLUSH_SENTINEL = object()


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


def resolve_settings(config: Config, **overrides: object) -> DictateSettings:
    """Merge config values with CLI overrides into a DictateSettings."""
    prompt = (overrides.get("prompt") or config.dictate_prompt or "").strip()
    if not prompt:
        prompt = DEFAULT_RUSSIAN_PROMPT
    return DictateSettings(
        language=(overrides.get("language") or config.dictate_language or "ru"),
        initial_prompt=prompt,
        idle_timeout=float(overrides.get("idle_timeout") or config.dictate_idle_timeout or 45),
        auto_stop_silence=float(
            overrides.get("auto_stop_silence", config.dictate_auto_stop_silence)
        ),
        hotkey=(overrides.get("hotkey") or config.dictate_hotkey or "<cmd>+<shift>+d"),
        trigger=(overrides.get("trigger") or config.dictate_trigger or "toggle").strip().lower(),
        vad_enabled=bool(overrides.get("vad", config.dictate_vad)),
        show_indicator=bool(overrides.get("show_indicator", config.dictate_show_indicator)),
        idle_visible=bool(overrides.get("idle_visible", config.dictate_idle_visible)),
        model=(overrides.get("model") or config.dictate_model or ""),
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
        self._stop_event = threading.Event()
        # Auto-stop request: set by the audio callback when silence elapses,
        # acted on by the run loop OFF the audio thread (the callback must
        # not call _end_session, which joins threads and can block).
        self._end_session_requested = False
        # Idle timeout: after a session ends, unload the model after this long.
        self._idle_timer: threading.Timer | None = None
        # Background threads.
        self._capture_thread: threading.Thread | None = None
        self._transcribe_thread: threading.Thread | None = None
        # VAD
        self._vad = VoiceActivityDetector() if settings.vad_enabled else None

    # ---------- public API ----------

    def run(self) -> int:
        """Start the engine and block until the user quits.

        Returns an exit code (0 = clean).
        """
        # Check permissions before starting.
        ok, hint = self.injector.check_permissions()
        if not ok:
            print(hint, file=sys.stderr)
            return 1

        # Main-thread indicator setup: platforms whose indicator needs
        # main-thread-only APIs (macOS NSWindow) create their UI here. run()
        # is called from the main thread, so this is always safe.
        self.indicator.setup()

        # Show the indicator in its dimmed idle state as soon as the service
        # starts, so the user can see dictation is armed. Without this the
        # NSPanel exists (created above) but is never ordered to the front
        # until a session begins — which is why the indicator was invisible
        # at idle. When idle_visible is off, keep the original behavior (hide
        # until a session starts).
        if self.s.show_indicator and self.s.idle_visible:
            self.indicator.set_state("idle")
            self.indicator.show()

        # On macOS with an indicator, run the AppKit event loop on the main
        # thread. Otherwise, run a plain blocking loop.
        if self.s.show_indicator and _is_macos():
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
            self.indicator.set_state("transcribing")
            self.indicator.show()
            self.stt.load()
        self.indicator.set_state("listening")
        self.indicator.show()
        with self._state_lock:
            if not self._session_active:
                # A stop/end raced in while we were loading — abort cleanly.
                self.indicator.set_state("idle")
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
            # Start the transcribe worker (drains the utterance queue).
            self._utterance_queue = queue.Queue()
            self._transcribe_thread = threading.Thread(
                target=self._transcribe_loop, daemon=True
            )
            self._transcribe_thread.start()
            # Start capturing audio.
            self._capturing = True
            self._capture_thread = threading.Thread(
                target=self._capture_loop, daemon=True
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
        """
        with self._state_lock:
            if not self._session_active:
                return
            self._session_active = False
            self._capturing = False
            capture_thread = self._capture_thread
            transcribe_thread = self._transcribe_thread

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
            self._capture_thread = None
            # Now safe to flush the buffer — the audio callback is gone.
            if self._utterance_buffer:
                self._utterance_queue.put(b"".join(self._utterance_buffer))
                self._utterance_buffer.clear()
                self._in_speech = False
                self._silence_frames = 0
            # Tell the transcribe worker to drain and exit after remaining work.
            self._utterance_queue.put(_FLUSH_SENTINEL)

        # Wait for the transcribe worker to finish pending utterances (off the
        # lock — this can take a while for long trailing utterances).
        if transcribe_thread and transcribe_thread.is_alive():
            transcribe_thread.join(timeout=30.0)
        with self._state_lock:
            self._transcribe_thread = None
        self.indicator.set_state("idle")
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

    def _capture_loop(self) -> None:
        """Background thread: open the mic stream and feed the audio callback.

        The sounddevice callback runs on its own audio thread and must not
        block. It only buffers frames, runs VAD, computes the indicator
        level, and enqueues completed utterances. Transcription happens on
        the separate ``_transcribe_loop`` thread.
        """
        try:
            import sounddevice as sd
        except ImportError:
            print(
                "sounddevice not installed. Install the dictate extra:\n"
                "  pipx inject whiz 'whiz[dictate]'",
                file=sys.stderr,
            )
            self._stop_event.set()
            return

        vad = self._vad
        frame_bytes = vad.frame_bytes if vad and vad.available else 960
        frame_samples = frame_bytes // 2
        frame_seconds = frame_samples / WHISPER_SAMPLE_RATE

        def callback(indata, frames, time_info, status):  # noqa: ARG001
            """sounddevice stream callback — runs on the audio thread."""
            if not self._capturing:
                return
            import numpy as np

            mono = indata[:, 0]
            # 16-bit PCM bytes for VAD.
            pcm = (mono * 32767).astype(np.int16).tobytes()
            # RMS amplitude for the indicator volume curve (0.0–1.0).
            rms = float(np.sqrt(np.mean(mono ** 2)))
            level = min(1.0, rms * 5.0)
            self.indicator.update_level(level)
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
                while self._capturing and not self._stop_event.is_set():
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
            # Clean up the session (hide indicator, flush) off the audio thread.
            self._end_session()
            return
        # The stream closed (hotkey toggle/PTT release stopped us) OR auto-stop
        # set the flag. Auto-stop must end the session here — off the audio thread,
        # so we don't block the sounddevice callback on a thread join.
        if self._end_session_requested and not self._stop_event.is_set():
            self._end_session_requested = False
            self._end_session()

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
            if _rms_int16(chunk) < _VAD_FRAME_ENERGY:
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

    # ---------- transcription worker ----------

    def _transcribe_loop(self) -> None:
        """Worker thread: drain the utterance queue, transcribe, inject."""
        import numpy as np

        while True:
            item = self._utterance_queue.get()
            if item is _FLUSH_SENTINEL:
                break
            self._transcribe_and_inject(item, np)

    def _transcribe_and_inject(self, pcm_bytes: bytes, np) -> None:
        """Transcribe a PCM utterance and inject the text into the focused app."""
        if len(pcm_bytes) < 2 * WHISPER_SAMPLE_RATE * _MIN_UTTERANCE_SECONDS:
            # Too short to be meaningful speech — skip.
            return
        # Energy gate: Whisper hallucinates training-data boilerplate on
        # near-silent / noise-only audio. Skip utterances below the RMS floor
        # before spending a transcription on them.
        energy = _rms_int16(pcm_bytes)
        if energy < _MIN_ENERGY:
            logger.debug("Skipping low-energy utterance (%.4f < %.4f)", energy, _MIN_ENERGY)
            return
        samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32) / 32768.0
        duration = len(samples) / WHISPER_SAMPLE_RATE
        logger.debug("Transcribing utterance: %.2fs, energy=%.4f", duration, energy)
        self.indicator.set_state("transcribing")
        try:
            text = self.stt.transcribe(
                samples,
                sample_rate=WHISPER_SAMPLE_RATE,
                language=self.s.language,
                initial_prompt=self.s.initial_prompt,
            )
        except Exception as e:  # noqa: BLE001
            logger.warning("Transcription failed: %s", e)
            self.indicator.set_state("listening")
            return
        if text.strip():
            # Hallucination safety net: suppress known Whisper training-data
            # artifacts that slip through when the audio is just above the
            # energy floor (e.g. steady fan/keyboard noise).
            text_lower = text.lower()
            if any(marker in text_lower for marker in _HALLUCINATION_PHRASES):
                logger.debug("Suppressing hallucination: %s", text)
                self.indicator.set_state("listening")
                return
            logger.debug("Injecting text: %s", text)
            self.injector.type_text(text)
        self.indicator.set_state("listening")

    # ---------- run loops ----------

    def _run_plain(self) -> int:
        """Run without AppKit — a blocking loop driving the hotkey listener."""
        listener = self._start_hotkey_listener()
        try:
            while not self._stop_event.is_set():
                time.sleep(_TICK)
        except KeyboardInterrupt:
            self.stop()
        finally:
            if listener:
                listener.stop()
        return 0

    def _run_with_appkit(self) -> int:
        """Run on the main thread with the macOS AppKit event loop (for the indicator)."""
        try:
            from AppKit import NSApplication, NSApplicationActivationPolicyAccessory
            from Foundation import NSDate
            from PyObjCTools import AppHelper
            from CoreFoundation import (
                CFRunLoopAddTimer,
                CFRunLoopGetMain,
                CFRunLoopTimerCreate,
                kCFRunLoopDefaultMode,
            )
        except ImportError:
            # No pyobjc — fall back to the plain loop (no indicator).
            return self._run_plain()

        app = NSApplication.sharedApplication()
        app.setActivationPolicy_(NSApplicationActivationPolicyAccessory)

        # Start the hotkey listener on a background thread.
        listener = self._start_hotkey_listener()

        def timer_callback(_timer, _info):  # noqa: ARG001
            if self._stop_event.is_set():
                app.terminate_(None)

        # Repeating timer that checks the stop event every 100 ms so
        # Ctrl+C / stop() can terminate the AppKit run loop.
        timer = CFRunLoopTimerCreate(
            None,
            NSDate.distantPast().timeIntervalSinceReferenceDate() + 0.1,
            0.1,  # interval
            0, 0,
            timer_callback,
            None,
        )
        CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopDefaultMode)

        try:
            AppHelper.runConsoleEventLoop(installInterrupt=True)
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
        try:
            from pynput import keyboard
        except ImportError:
            print(
                "pynput not installed — cannot listen for the hotkey.\n"
                "Install the dictate extra: pipx inject whiz 'whiz[dictate]'\n"
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
            return None

        if not parsed:
            print(f"Invalid hotkey '{self.s.hotkey}': empty combo", file=sys.stderr)
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

    # Override the STT model if a custom one was passed.
    if settings.model and hasattr(stt, "_model_ref"):
        stt._model_ref = settings.model  # noqa: SLF001

    engine = DictationEngine(settings, stt, injector, indicator)
    model_name = getattr(stt, "_model_ref", "?")
    trigger_label = "push-to-talk" if settings.trigger == "ptt" else "toggle"
    print(
        f"whiz dictate — {trigger_label}: {settings.hotkey}  |  model: {model_name}  "
        f"|  language: {settings.language}",
        file=sys.stderr,
    )
    if settings.trigger == "ptt":
        print("Hold the hotkey to dictate; release to stop. Ctrl+C to quit.", file=sys.stderr)
    else:
        print("Press the hotkey to start/stop dictation. Ctrl+C to quit.", file=sys.stderr)
    return engine.run()
