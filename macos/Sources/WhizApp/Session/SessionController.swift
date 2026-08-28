import Foundation
import Combine

/// Owns dictation session state and drives the UI.
///
/// This is the Swift counterpart to `DictationEngine` in
/// `whiz/dictate/engine.py`. At this stage it deliberately implements only the
/// half that does not need speech recognition: trigger handling, state
/// transitions, and the mic level feed that animates the indicator. The STT
/// path is not wired up yet — see `transcribeAndInject()`.
///
/// Everything here is main-actor isolated. The Python version needed explicit
/// locks and `performSelectorOnMainThread:` dispatch because its hotkey, audio,
/// and transcribe threads all mutated shared state; pinning the controller to
/// the main actor gets the same guarantee from the compiler instead.
/// Uses `ObservableObject` rather than the newer `@Observable` macro, which is
/// macOS 14+. Deployment target is 13 (see Package.swift), so do not "modernise"
/// this without also raising the floor and dropping 2017-era Macs.
@MainActor
final class SessionController: ObservableObject {

    @Published private(set) var state: DictationState = .idle
    @Published private(set) var isSessionActive = false

    /// Live mic amplitude in 0...1, feeding the pill's waveform bars.
    @Published private(set) var level: Double = 0.0

    var config: WhizConfig

    private let mic = MicLevelMonitor()

    init(config: WhizConfig = .load()) {
        self.config = config
    }

    // MARK: - Trigger

    func toggleSession() {
        isSessionActive ? endSession() : startSession()
    }

    func startSession() {
        guard !isSessionActive else { return }
        isSessionActive = true
        state = .listening

        mic.start { [weak self] amplitude in
            // MicLevelMonitor hands us amplitude on an audio thread; hop to the
            // main actor before touching observable state.
            Task { @MainActor in self?.level = amplitude }
        }
    }

    func endSession() {
        guard isSessionActive else { return }
        mic.stop()
        isSessionActive = false
        level = 0.0
        state = .idle
    }

    // MARK: - STT seam

    /// Where speech recognition will plug in.
    ///
    /// The shape this needs to take, ported from `engine.py`:
    ///   1. VAD segments the mic stream into utterances (`dictate/vad.py`)
    ///   2. Reject utterances under `_MIN_UTTERANCE_SECONDS` / `_MIN_ENERGY`
    ///      against the adaptive noise floor
    ///   3. Transcribe with `config.language` + `config.prompt`
    ///   4. Drop known hallucination phrases (`_HALLUCINATION_PHRASES`)
    ///   5. `TextInjector.type(text)` into the focused app
    ///
    /// Steps 2 and 4 are tuned behaviour, not boilerplate — port the constants
    /// across rather than re-deriving them.
    func transcribeAndInject(_ pcm: [Float]) {
        fatalError("STT not wired up yet — see docs/SWIFT-APP.md, phase 2")
    }
}
