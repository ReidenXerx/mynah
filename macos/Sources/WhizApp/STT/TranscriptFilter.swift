import Foundation

/// Rejects Whisper's known failure output before it reaches the user's keyboard.
///
/// Every constant here is ported from `whiz/dictate/engine.py`, and they are
/// *measured* values, not defaults — tuned against a real mic in a real room
/// with a MacBook cooler running. Do not tidy them without re-testing.
///
/// Caveat worth keeping in mind: whisper.cpp reimplemented Whisper's decoding
/// loop independently of the reference implementation that mlx-whisper ports,
/// so the exact hallucination phrases it emits on silence may differ from this
/// list. The list is a starting point carried over from a year of mlx use, not
/// a guarantee. Add to it as testing turns up new artifacts.
enum TranscriptFilter {

    // The duration and energy floors that used to live here were dead: the
    // gating moved to `SessionController.enqueue`, driven by
    // `dictate_min_utterance` / `dictate_min_energy` / `dictate_frame_energy`.
    // Keeping them meant two sources of truth, and the copy here still held the
    // old, too-high values (0.35 / 0.025 / 0.03) that the config keys were added
    // to replace — so wiring them back up would have silently restored the
    // "you have to shout" behaviour. Defaults now live in `whiz/config.py` and
    // `WhizConfig`.

    /// Silence that ends an utterance within a session. Shorter is snappier but
    /// clips slow speech; longer feels natural but delays the text appearing.
    static let utteranceSilence: Double = 0.8

    // Adaptive noise floor. The static thresholds above suit a quiet room; in a
    // noisy one, steady background noise exceeds them and gets misclassified as
    // speech, seeding exactly the near-silence hallucinations we are guarding
    // against. Measuring ambient level at session start and scaling the gates
    // keeps the static values as floors while adapting upward when needed.
    static let noiseCalibrationDuration: Double = 1.0
    static let noiseFrameMultiplier: Double = 3.5   // ~11 dB above the floor
    static let noiseUtteranceMultiplier: Double = 3.0  // ~10 dB above the floor
    static let noiseMinimumSamples = 5

    /// Calibration frames at or above this RMS are speech, not noise —
    /// excluded from the median regardless of the gates in force. An
    /// absolute discrimination line, not a gate: the legacy measured
    /// per-frame floor (0.03 ≈ -30 dB), between real MacBook-cooler
    /// noise (~0.02) and quiet speech (~0.04). If fewer than
    /// `noiseMinimumSamples` quiet frames remain, calibration aborts to
    /// the static gates — the median must never be measured on speech
    /// (that poisoned the floor and silently dropped the first word).
    static let calibrationSpeechFloor: Double = 0.03

    /// Known Whisper hallucination phrases, lowercased.
    ///
    /// Fed silence or noise, Whisper emits training-data artifacts — mostly
    /// Russian subtitle-credit boilerplate, since that is what dominates its
    /// Russian training data. Matching is substring-based on the lowercased
    /// transcript.
    static let hallucinationPhrases: Set<String> = [
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
        // Observed specifically on MacBook cooler/fan noise.
        "субтитры",
        "следите за обновлениями",
        "оставайтесь с нами",
        "не забудьте подписаться",
        "вы можете поддержать",
    ]

    /// RMS amplitude of normalised float samples, 0...1.
    static func rms(_ samples: [Float]) -> Double {
        guard !samples.isEmpty else { return 0 }
        var sum: Double = 0
        for sample in samples { sum += Double(sample) * Double(sample) }
        return (sum / Double(samples.count)).squareRoot()
    }

    /// Whether transcribed text is a hallucination and should be dropped.
    static func isHallucination(_ text: String) -> Bool {
        let normalized = text.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
        if normalized.isEmpty { return true }
        return hallucinationPhrases.contains { normalized.contains($0) }
    }
}
