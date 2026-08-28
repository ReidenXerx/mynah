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

    /// Minimum utterance length. Shorter than this is a click, breath or knock.
    static let minimumDuration: Double = 0.35

    /// Minimum RMS energy (normalised 0...1) to bother transcribing.
    /// ~-32 dB — above a typical Mac room noise floor and cooler noise. Whisper
    /// hallucinates training-data boilerplate on near-silent input, so the
    /// cheapest defence is never sending it silence.
    static let minimumEnergy: Double = 0.025

    /// Per-frame energy floor, applied *before* VAD. webrtcvad and Silero alike
    /// can classify steady low-level noise (fan, HVAC, keyboard) as speech;
    /// this stops those frames from ever starting or extending an utterance.
    static let frameEnergyFloor: Double = 0.03

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

    /// Whether an utterance is worth sending to Whisper at all.
    static func isWorthTranscribing(
        samples: [Float],
        sampleRate: Double,
        energyThreshold: Double
    ) -> Bool {
        let duration = Double(samples.count) / sampleRate
        guard duration >= minimumDuration else { return false }
        return rms(samples) >= energyThreshold
    }

    /// Whether transcribed text is a hallucination and should be dropped.
    static func isHallucination(_ text: String) -> Bool {
        let normalized = text.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
        if normalized.isEmpty { return true }
        return hallucinationPhrases.contains { normalized.contains($0) }
    }
}
