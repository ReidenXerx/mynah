import Combine
import Foundation

/// Owns dictation session state and drives the UI.
///
/// The Swift counterpart to `DictationEngine` in `whiz/dictate/engine.py`, and
/// it keeps that file's threading contract: audio capture never blocks on
/// transcription. The tap thread only buffers and segments; a detached task
/// transcribes and injects. Blocking the tap causes dropouts.
///
/// Uses `ObservableObject` rather than the newer `@Observable` macro, which is
/// macOS 14+. Deployment target is 13 (see Package.swift), so do not "modernise"
/// this without also raising the floor and dropping 2017-era Macs.
@MainActor
final class SessionController: ObservableObject {

    @Published private(set) var state: DictationState = .idle
    @Published private(set) var isSessionActive = false

    /// Live mic amplitude in 0...1, feeding the pill's waveform bars.
    @Published private(set) var level: Double = 0.0

    /// Surfaced in the menu so failures are visible rather than silent.
    @Published private(set) var lastError: String?

    /// Live Accessibility state.
    ///
    /// Polled rather than read once: the menu used to capture this into `@State`
    /// at first render, so granting the permission never updated the display —
    /// it read "not granted" forever, which looked exactly like the grant having
    /// failed. TCC sends no notification, so polling is the only option.
    @Published private(set) var isAccessibilityTrusted = Permissions.isAccessibilityTrusted

    var config: WhizConfig

    private let capture = AudioCapture()
    private var whisper: WhisperEngine?
    private lazy var detector = makeDetector()
    private var idleUnloadTask: Task<Void, Never>?

    /// Serialises transcription so utterances are injected in the order spoken.
    /// Without this, a short utterance following a long one could finish first
    /// and type its text ahead of the earlier sentence.
    private var transcriptionChain: Task<Void, Never> = Task {}

    init(config: WhizConfig = .load()) {
        self.config = config
    }

    // MARK: - Trigger

    func toggleSession() {
        isSessionActive ? endSession() : startSession()
    }

    func startSession() {
        guard !isSessionActive else { return }
        idleUnloadTask?.cancel()
        idleUnloadTask = nil
        lastError = nil
        Task { await beginSession() }
    }

    /// Async because a cold model load takes seconds. `engine.py` did this
    /// synchronously; on the main thread that would beachball the menu bar.
    private func beginSession() async {
        // Load before claiming the session is live, so a missing model surfaces
        // immediately rather than after the user has spoken a whole sentence
        // into a dead session.
        do {
            try await ensureModelLoaded()
        } catch {
            Log.stt.error("model load failed: \(error.localizedDescription, privacy: .public)")
            lastError = error.localizedDescription
            state = .idle
            return
        }
        // Ask for the microphone before starting capture. Granting it while
        // the engine is already running yields silence for the whole session.
        guard await Permissions.requestMicrophone() else {
            Log.session.error("microphone permission denied")
            lastError = "Microphone access is required. Enable whiz in "
                + "System Settings → Privacy & Security → Microphone."
            state = .idle
            return
        }
        // Warn but continue: recognition still works and is worth seeing, but
        // nothing will reach the focused app until this is granted.
        refreshPermissions()
        if !isAccessibilityTrusted {
            Log.session.error("Accessibility not granted — transcription will not be typed")
            lastError = "Accessibility not granted — text cannot be typed. "
                + "Use \"Grant Accessibility…\" above."
        }
        guard !isSessionActive else { return }

        detector = makeDetector()
        isSessionActive = true
        state = .listening

        do {
            try capture.start { [weak self] samples, level in
                // Audio thread. Hop to the main actor before touching state.
                Task { @MainActor in self?.ingest(samples, level: level) }
            }
            Log.session.notice("session started")
        } catch {
            Log.audio.error("capture failed: \(error.localizedDescription, privacy: .public)")
            lastError = error.localizedDescription
            isSessionActive = false
            state = .idle
        }
    }

    func endSession() {
        guard isSessionActive else { return }
        capture.stop()
        isSessionActive = false
        level = 0

        // Whatever is still buffered is real speech the user just finished.
        if let final = detector.flush() {
            enqueue(final)
        }
        state = .idle
        scheduleIdleUnload()
    }

    // MARK: - Audio

    private func ingest(_ samples: [Float], level: Double) {
        self.level = level
        guard let utterance = detector.process(samples) else { return }
        enqueue(utterance)
    }

    private func makeDetector() -> UtteranceDetector {
        UtteranceDetector(
            sampleRate: WhisperEngine.sampleRate,
            frameFloor: config.frameEnergy,
            utteranceFloor: config.minEnergy)
    }

    /// Surface a failure raised outside the controller (e.g. hotkey registration).
    func reportError(_ message: String) {
        lastError = message
    }

    /// Re-read permission state. Called on a timer from `AppDelegate`.
    func refreshPermissions() {
        let trusted = Permissions.isAccessibilityTrusted
        guard trusted != isAccessibilityTrusted else { return }
        isAccessibilityTrusted = trusted
        Log.ui.notice("accessibility trust changed: \(trusted, privacy: .public)")
        // Clear the stale complaint as soon as the grant lands, so the menu does
        // not keep accusing the user of something they have already done.
        if trusted, lastError?.contains("Accessibility") == true {
            lastError = nil
        }
    }

    private func enqueue(_ utterance: UtteranceDetector.Utterance) {
        // Both rejections below used to `return` silently, which is why the
        // first real test logged "utterance 0.90s" and then nothing at all.
        let energy = TranscriptFilter.rms(utterance.samples)
        let gate = detector.currentEnergyThreshold
        Log.session.notice(
            "utterance \(utterance.duration, format: .fixed(precision: 2))s rms \(energy, format: .fixed(precision: 4)) gate \(gate, format: .fixed(precision: 4))")

        guard utterance.duration >= config.minUtterance else {
            Log.session.notice("utterance rejected: shorter than dictate_min_utterance")
            return
        }
        guard energy >= gate else {
            Log.session.notice("utterance rejected: below the energy gate")
            return
        }

        guard let whisper else {
            Log.session.error("utterance dropped: no model loaded")
            return
        }
        let language = config.language
        let prompt = config.prompt.isEmpty ? DefaultPrompt.russian : config.prompt

        state = .transcribing
        let samples = utterance.samples
        let previous = transcriptionChain
        transcriptionChain = Task { [weak self] in
            // Await the previous utterance so text is injected in the order it
            // was spoken. Actor isolation serialises access to the whisper
            // context but says nothing about ordering.
            _ = await previous.result
            do {
                let text = try await whisper.transcribe(
                    samples: samples, language: language, prompt: prompt)
                self?.deliver(.success(text))
            } catch {
                self?.deliver(.failure(error))
            }
        }
    }

    private func deliver(_ result: Result<String, Error>) {
        switch result {
        case .failure(let error):
            Log.stt.error("transcribe failed: \(error.localizedDescription, privacy: .public)")
            lastError = error.localizedDescription
        case .success(let text):
            // The last line of defence: energy gating misses low-but-audible
            // noise that the decoder then turns into subtitle credits.
            if TranscriptFilter.isHallucination(text) {
                Log.stt.notice("dropped hallucination: \(text, privacy: .public)")
            } else {
                Log.stt.notice("injecting \(text.count) chars")
                TextInjector.type(text)
            }
        }
        state = isSessionActive ? .listening : .idle
    }

    // MARK: - Model lifecycle

    private func ensureModelLoaded() async throws {
        if let whisper, await whisper.isLoaded { return }
        guard let modelURL = WhisperModel.resolve(configured: config.model) else {
            throw WhisperError.noModelFound
        }
        let engine = whisper ?? WhisperEngine(modelURL: modelURL)
        try await engine.load()
        whisper = engine
    }

    /// Keep the model resident for `dictate_idle_timeout` so back-to-back
    /// dictation stays warm, then free it — the "zero RAM at idle" behaviour
    /// from `engine.py`. A timeout of 0 means never unload.
    private func scheduleIdleUnload() {
        idleUnloadTask?.cancel()
        let timeout = config.idleTimeout
        guard timeout > 0 else { return }

        idleUnloadTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
            guard !Task.isCancelled else { return }
            await self?.unloadModel()
        }
    }

    private func unloadModel() async {
        guard !isSessionActive else { return }
        await whisper?.unload()
        whisper = nil
    }
}

/// Default `initial_prompt` when `dictate_prompt` is unset.
///
/// Carried over verbatim from `DEFAULT_RUSSIAN_PROMPT` in `engine.py`. Informal
/// Russian in the prompt biases the decoder toward reproducing informal Russian
/// verbatim instead of sanitising it — Whisper treats the prompt as prior
/// context, so seeing obscenity makes it far likelier to transcribe obscenity
/// rather than censoring it. Removing this silently degrades the product.
enum DefaultPrompt {
    static let russian = """
        Это разговорная запись с неформальной лексикой, сленгом и матом. \
        Запиши всё как есть, без цензуры: пиздец, охуенно, хуйня, ебать, \
        заебись, бля, сука, хуй, пизда, мудак.
        """
}
