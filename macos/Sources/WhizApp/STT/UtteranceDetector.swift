import Foundation

/// Segments a continuous mic stream into utterances.
///
/// Ports the segmentation half of `engine.py`'s audio callback. Two gates in
/// series, and the order matters: the per-frame energy floor runs *first*, so
/// steady low-level noise never reaches the speech decision at all. `engine.py`
/// learned this the hard way — webrtcvad classified MacBook fan noise as speech,
/// producing utterances that passed the whole-buffer RMS check and then
/// hallucinated subtitle credits.
///
/// This is energy-based rather than model-based. `engine.py` layered webrtcvad
/// on top; whisper.cpp ships Silero VAD (and `whiz models download-vad` already
/// fetches the model), so that is the natural upgrade once the basic loop is
/// validated. See `docs/SWIFT-APP.md`.
struct UtteranceDetector {

    /// Emitted when silence closes an utterance.
    struct Utterance {
        var samples: [Float]
        var duration: Double
    }

    /// Trailing silence kept when closing an utterance. Enough that word
    /// endings are not clipped, without dragging the whole-buffer RMS down.
    private static let trailingPadding: Double = 0.2

    private let sampleRate: Double
    private var buffer: [Float] = []
    private var silentDuration: Double = 0
    private var isSpeaking = false

    // Adaptive noise floor, measured over the first second of a session.
    private var calibrationSamples: [Double] = []
    private var calibratedDuration: Double = 0
    private var frameThreshold: Double
    private var utteranceThreshold: Double

    /// Static floors come from config so sensitivity is tunable without a
    /// rebuild — the shipped defaults were far too high on some microphones,
    /// requiring the user to raise their voice.
    private let frameFloor: Double
    private let utteranceFloor: Double

    init(sampleRate: Double, frameFloor: Double, utteranceFloor: Double) {
        self.sampleRate = sampleRate
        self.frameFloor = frameFloor
        self.utteranceFloor = utteranceFloor
        self.frameThreshold = frameFloor
        self.utteranceThreshold = utteranceFloor
    }

    /// The utterance energy gate, raised if the room turned out to be noisy.
    var currentEnergyThreshold: Double { utteranceThreshold }

    /// Feed one buffer of samples. Returns an utterance when silence closes one.
    mutating func process(_ frame: [Float]) -> Utterance? {
        guard !frame.isEmpty else { return nil }

        let frameDuration = Double(frame.count) / sampleRate
        let energy = TranscriptFilter.rms(frame)

        // Sample the ambient level, but keep processing this frame.
        //
        // This used to `return nil` during the calibration window, which threw
        // away the first second of every session — so anyone who pressed the
        // hotkey and started talking immediately lost their opening word.
        // `engine.py` collects the sample and falls through to segmentation in
        // the same frame; this now matches. The gates in use during the window
        // are the static floors, exactly as in Python, until `applyCalibration`
        // raises them.
        //
        // Speech during calibration does skew the measured noise floor, which is
        // why the floor is a median rather than a mean — a few loud frames among
        // ~30 do not move it.
        if calibratedDuration < TranscriptFilter.noiseCalibrationDuration {
            calibrationSamples.append(energy)
            calibratedDuration += frameDuration
            if calibratedDuration >= TranscriptFilter.noiseCalibrationDuration {
                applyCalibration()
            }
        }

        if energy >= frameThreshold {
            isSpeaking = true
            silentDuration = 0
            buffer.append(contentsOf: frame)
            return nil
        }

        guard isSpeaking else { return nil }

        // Trailing silence still belongs to the utterance — cutting at the
        // exact frame speech drops clips word endings.
        buffer.append(contentsOf: frame)
        silentDuration += frameDuration
        guard silentDuration >= TranscriptFilter.utteranceSilence else { return nil }

        let utterance = makeUtterance()
        reset()
        return utterance
    }

    /// Build the utterance, dropping most of the trailing silence.
    ///
    /// The buffer accumulates every silent frame until the 0.8 s threshold is
    /// reached, so a short phrase ends up as ~0.1 s of speech followed by 0.8 s
    /// of nothing. Gating on RMS across all of that averages the speech away and
    /// the utterance is silently discarded — which is exactly what happened in
    /// the first real test: "utterance 0.90s" logged, then nothing.
    private func makeUtterance() -> Utterance {
        let keep = Int(Self.trailingPadding * sampleRate)
        let drop = max(0, Int(silentDuration * sampleRate) - keep)
        let trimmed = drop > 0 && drop < buffer.count
            ? Array(buffer[0..<(buffer.count - drop)])
            : buffer
        return Utterance(samples: trimmed, duration: Double(trimmed.count) / sampleRate)
    }

    /// Close out whatever is buffered — called when the session ends so a final
    /// utterance is not lost to the user releasing the key mid-sentence.
    mutating func flush() -> Utterance? {
        guard isSpeaking, !buffer.isEmpty else { return nil }
        let utterance = makeUtterance()
        reset()
        return utterance
    }

    private mutating func reset() {
        buffer.removeAll(keepingCapacity: true)
        silentDuration = 0
        isSpeaking = false
    }

    /// Raise both gates proportionally to measured ambient noise, using the
    /// median rather than the mean so a transient spike (or a word spoken
    /// during calibration) does not skew the floor.
    private mutating func applyCalibration() {
        guard calibrationSamples.count >= TranscriptFilter.noiseMinimumSamples else { return }
        let sorted = calibrationSamples.sorted()
        let noiseFloor = sorted[sorted.count / 2]

        frameThreshold = max(frameFloor, noiseFloor * TranscriptFilter.noiseFrameMultiplier)
        utteranceThreshold = max(utteranceFloor, noiseFloor * TranscriptFilter.noiseUtteranceMultiplier)
        // os_log interpolation is an autoclosure and cannot capture `self`
        // inside a mutating method, so read the values into locals first.
        let frame = frameThreshold
        let utterance = utteranceThreshold
        Log.audio.notice(
            "noise floor \(noiseFloor, format: .fixed(precision: 4)) -> frame gate \(frame, format: .fixed(precision: 4)), utterance gate \(utterance, format: .fixed(precision: 4))")
    }
}
