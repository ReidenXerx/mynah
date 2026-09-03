import Testing
import Foundation
@testable import WhizApp

/// The shared tuning contract — the Swift half of the cross-platform pins.
///
/// `tuning/tuning.toml` is the single source of truth for the dictate
/// segmentation pipeline's constants. This suite pins the compiled-in
/// Swift constants against that file, exactly as
/// `tests/test_tuning.py` pins the Python ones, and runs the shared
/// golden-corpus fixtures (`tuning/golden/*.wav` + `expected.json`)
/// through `UtteranceDetector` exactly as
/// `tests/test_segmentation_golden.py` drives the Python engine.
///
/// The tuning file is deliberately NOT read at runtime: if a value
/// changes there, these tests fail until every implementation is
/// updated — that is the contract working as designed.
@Suite("Tuning contract")
struct TuningTests {

    private enum FixtureError: Error, CustomStringConvertible {
        case malformed(String)
        var description: String {
            switch self {
            case .malformed(let msg): return msg
            }
        }
    }

    // MARK: - Fixture location

    /// …/macos/Tests/WhizAppTests/TuningTests.swift → repo root.
    private static let repoRoot = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()   // WhizAppTests
        .deletingLastPathComponent()   // Tests
        .deletingLastPathComponent()   // macos
        .deletingLastPathComponent()   // repo root

    private static var tuningURL: URL {
        repoRoot.appendingPathComponent("tuning/tuning.toml")
    }
    private static var goldenDir: URL {
        repoRoot.appendingPathComponent("tuning/golden")
    }

    private static func tuning() throws -> [String: FlatTOML.Value] {
        let text = try String(contentsOf: tuningURL, encoding: .utf8)
        return FlatTOML.parse(text)
    }

    /// Numeric value for a key, accepting int-or-double the way
    /// WhizConfig.number does (Python writes both shapes).
    private static func number(_ values: [String: FlatTOML.Value], _ key: String) throws -> Double {
        switch values[key] {
        case .double(let d)?: return d
        case .int(let i)?: return Double(i)
        default:
            throw FixtureError.malformed("tuning.toml: key '\(key)' is missing or not numeric")
        }
    }

    // MARK: - Constant pins (mirror of tests/test_tuning.py)

    @Test("segmentation and calibration constants match tuning.toml")
    func constantsMatchTuning() throws {
        let t = try Self.tuning()
        // `try` cannot sit on the RHS of `==` once #expect re-emits the
        // expression, so every value is hoisted first.
        let utteranceSilence = try Self.number(t, "utterance_silence")
        let calDuration = try Self.number(t, "noise_calibration_seconds")
        let frameMult = try Self.number(t, "noise_frame_multiplier")
        let uttMult = try Self.number(t, "noise_utterance_multiplier")
        let minSamples = try Self.number(t, "noise_min_samples")
        let calSpeechFloor = try Self.number(t, "calibration_speech_floor")
        let trailingPadding = try Self.number(t, "trailing_padding")
        #expect(TranscriptFilter.utteranceSilence == utteranceSilence)
        #expect(TranscriptFilter.noiseCalibrationDuration == calDuration)
        #expect(TranscriptFilter.noiseFrameMultiplier == frameMult)
        #expect(TranscriptFilter.noiseUtteranceMultiplier == uttMult)
        #expect(Double(TranscriptFilter.noiseMinimumSamples) == minSamples)
        #expect(TranscriptFilter.calibrationSpeechFloor == calSpeechFloor)
        #expect(UtteranceDetector.trailingPadding == trailingPadding)
    }

    @Test("WhizConfig defaults match tuning.toml")
    func configDefaultsMatchTuning() throws {
        let t = try Self.tuning()
        let frameEnergy = try Self.number(t, "frame_energy_default")
        let minEnergy = try Self.number(t, "min_energy_default")
        let minUtterance = try Self.number(t, "min_utterance_default")
        let config = WhizConfig()
        #expect(config.frameEnergy == frameEnergy)
        #expect(config.minEnergy == minEnergy)
        #expect(config.minUtterance == minUtterance)
    }

    @Test("hallucination phrases match tuning.toml (set equality)")
    func hallucinationPhrasesMatchTuning() throws {
        let t = try Self.tuning()
        guard case .stringArray(let phrases)? = t["hallucination_phrases"] else {
            throw FixtureError.malformed(
                "tuning.toml: hallucination_phrases is not a string array — "
                + "if this fails, FlatTOML lost the multi-line array")
        }
        #expect(Set(phrases) == TranscriptFilter.hallucinationPhrases)
        // Set equality would hide a duplicated entry in the file; count pins it.
        #expect(phrases.count == TranscriptFilter.hallucinationPhrases.count)
    }

    @Test("tuning.toml parses with exactly the contract keys")
    func tuningFileParsesCompletely() throws {
        let t = try Self.tuning()
        let expected: Set<String> = [
            "utterance_silence",
            "trailing_padding",
            "noise_calibration_seconds",
            "noise_frame_multiplier",
            "noise_utterance_multiplier",
            "noise_min_samples",
            "calibration_speech_floor",
            "frame_energy_default",
            "min_energy_default",
            "min_utterance_default",
            "hallucination_phrases",
        ]
        #expect(Set(t.keys) == expected)
    }

    // MARK: - FlatTOML multi-line arrays (the tuning file's shape)

    // tuning.toml carries the hallucination phrase list as a multi-line
    // TOML array with a comment inside it. FlatTOML's parser used to be
    // strictly line-based, so the key silently vanished — and with it,
    // every tuning pin above. These tests pin the fix.

    @Test("multi-line arrays parse, skipping comments inside")
    func flatTOMLParsesMultiLineArrays() {
        let values = FlatTOML.parse("""
        phrases = [
            "alpha",
            # a comment inside the array
            "beta",
        ]
        next_key = 3
        """)
        #expect(values["phrases"] == .stringArray(["alpha", "beta"]))
        #expect(values["next_key"] == .int(3))
    }

    @Test("an unterminated array drops only its own key")
    func flatTOMLUnterminatedArrayIsContain() {
        let values = FlatTOML.parse("""
        broken = [
        next_key = 7
        """)
        #expect(values["broken"] == nil)
        #expect(values["next_key"] == .int(7))
    }

    @Test("a multi-line array closing on the first line is untouched")
    func flatTOMLSingleLineArrayStillWorks() {
        let values = FlatTOML.parse("model_dirs = [\"/one\", \"/two\"]\nplain = 1\n")
        #expect(values["model_dirs"] == .stringArray(["/one", "/two"]))
        #expect(values["plain"] == .int(1))
    }

    // MARK: - Golden segmentation corpus (mirror of tests/test_segmentation_golden.py)

    private static let sampleRate = 16000.0
    private static let frameLen = 480        // 30 ms — the contract's frame size
    private static let frameSeconds = 0.03

    /// Minimal RIFF/WAVE reader for the corpus's 16 kHz mono s16le files.
    private static func loadWavSamples(_ name: String) throws -> [Float] {
        let url = goldenDir.appendingPathComponent("\(name).wav")
        let data = try Data(contentsOf: url)
        guard data.count >= 12,
              String(bytes: data[0..<4], encoding: .ascii) == "RIFF",
              String(bytes: data[8..<12], encoding: .ascii) == "WAVE" else {
            throw FixtureError.malformed("\(name).wav: not a RIFF/WAVE file")
        }
        var offset = 12
        while offset + 8 <= data.count {
            let id = String(bytes: data[offset..<offset + 4], encoding: .ascii) ?? ""
            let size = Int(data[offset + 4])
                | Int(data[offset + 5]) << 8
                | Int(data[offset + 6]) << 16
                | Int(data[offset + 7]) << 24
            let start = offset + 8
            if id == "data" {
                let end = min(start + size, data.count)
                var samples: [Int16] = []
                samples.reserveCapacity((end - start) / 2)
                var i = start
                while i + 1 < end {
                    samples.append(Int16(bitPattern: UInt16(data[i]) | UInt16(data[i + 1]) << 8))
                    i += 2
                }
                return samples.map { Float($0) / 32768.0 }
            }
            offset = start + size + (size % 2)   // chunks are word-aligned
        }
        throw FixtureError.malformed("\(name).wav: no data chunk")
    }

    private struct ExpectedRegion {
        var start: Double
        var end: Double
        var rejectedByEnergyGate: Bool
    }

    private static func loadExpected(_ name: String) throws -> [ExpectedRegion] {
        let url = goldenDir.appendingPathComponent("expected.json")
        let data = try Data(contentsOf: url)
        let obj = try JSONSerialization.jsonObject(with: data)
        guard let all = obj as? [String: [[String: Any]]],
              let regions = all[name] else {
            throw FixtureError.malformed("expected.json: no entry for \(name)")
        }
        return try regions.map { r in
            guard let s = (r["start"] as? NSNumber)?.doubleValue,
                  let e = (r["end"] as? NSNumber)?.doubleValue,
                  let rejected = (r["rejected_by_energy_gate"] as? NSNumber)?.boolValue else {
                throw FixtureError.malformed("expected.json: malformed region for \(name)")
            }
            return ExpectedRegion(start: s, end: e, rejectedByEnergyGate: rejected)
        }
    }

    /// Frames needed to close an utterance: smallest n with n*frame ≥ silence.
    /// 0.8 / 0.03 → 27 — the same integer the detector reaches by accumulation.
    private static var closeSilenceFrames: Int {
        Int((TranscriptFilter.utteranceSilence / frameSeconds).rounded(.up))
    }

    /// The corpus's pinned cases — a single list so the orphan guard below
    /// can hold it against expected.json's keys.
    private static let goldenCases = [
        "quiet_two_utterances",
        "speech_during_calibration",
        "speech_late_in_calibration",
        "noisy_room",
        "click_below_min",
        "trailing_silence_trim",
        "gap_below_silence",
        "speech_over_noise_in_calibration",
    ]

    @Test("UtteranceDetector segments the golden corpus as pinned",
          arguments: TuningTests.goldenCases)
    func goldenSegmentation(name: String) throws {
        let samples = try Self.loadWavSamples(name)
        let expected = try Self.loadExpected(name)

        var detector = UtteranceDetector(
            sampleRate: Self.sampleRate,
            frameFloor: 0.010,
            utteranceFloor: 0.008)

        // Region bookkeeping, reconstructed in frame space (NOT from
        // Int(silentDuration * rate) — the accumulated silence can be off
        // by one sample in floating point, which would shift the start):
        // - end = start time of the frame that closed the region (the loop
        //   index at emission), matching expected.json's `end`.
        // - the buffer spans speechFrames + closeSilenceFrames frames; the
        //   emitted samples are that buffer trimmed to trailing_padding of
        //   kept silence, so speechFrames = (count - kept) / 480 by floor
        //   division, robust to ±1 sample of trim.
        let keep = Int(UtteranceDetector.trailingPadding * Self.sampleRate)
        var regions: [(start: Double, end: Double, rejected: Bool)] = []
        var frameIndex = 0
        var i = 0
        while i + Self.frameLen <= samples.count {
            let frame = Array(samples[i..<i + Self.frameLen])
            if let utterance = detector.process(frame) {
                let speechFrames = max(0, (utterance.samples.count - keep) / Self.frameLen)
                let bufferedFrames = speechFrames + Self.closeSilenceFrames
                let startIdx = frameIndex - bufferedFrames + 1
                // The energy gate exactly as SessionController.enqueue applies it.
                let rejected = TranscriptFilter.rms(utterance.samples) < detector.currentEnergyThreshold
                regions.append((
                    Double(startIdx) * Self.frameSeconds,
                    Double(frameIndex) * Self.frameSeconds,
                    rejected))
            }
            frameIndex += 1
            i += Self.frameLen
        }

        // No fixture ends mid-speech: nothing may remain buffered.
        #expect(detector.flush() == nil, "\(name): fixture ended mid-speech?")

        #expect(regions.count == expected.count, """
            \(name): UtteranceDetector produced \(regions.count) region(s), \
            expected \(expected.count): \
            \(regions.map { "(\($0.start), \($0.end), rejected=\($0.rejected))" })
            """)
        for (got, exp) in zip(regions, expected) {
            #expect(abs(got.start - exp.start) < 1e-9,
                    "\(name): region start \(got.start) != expected \(exp.start)")
            #expect(abs(got.end - exp.end) < 1e-9,
                    "\(name): region end \(got.end) != expected \(exp.end)")
            #expect(got.rejected == exp.rejectedByEnergyGate, """
                \(name): energy-gate verdict \(got.rejected) != expected \
                \(exp.rejectedByEnergyGate) (RMS vs calibrated gate)
                """)
        }
    }

    @Test("expected.json holds exactly the pinned cases")
    func expectedJsonHoldsExactlyThePinnedCases() throws {
        // Orphan guard: a case dropped from goldenCases would silently stop
        // being tested (the parametrized test visits goldenCases only), and
        // a stray expected.json entry would never be noticed — loadExpected
        // throws only for a MISSING entry, never for an extra one.
        let url = Self.goldenDir.appendingPathComponent("expected.json")
        let data = try Data(contentsOf: url)
        let obj = try JSONSerialization.jsonObject(with: data)
        guard let all = obj as? [String: Any] else {
            throw FixtureError.malformed("expected.json: not a map of case -> regions")
        }
        #expect(Set(all.keys) == Set(Self.goldenCases))
    }

    @Test("speech in the calibration window aborts to the static gates — the first word survives")
    func speechDuringCalibrationKeepsStaticGates() throws {
        // speech_during_calibration pins the speech-aware calibration
        // fix: speech fills the 1 s window, the speech frames are
        // excluded from the noise median, too few quiet frames remain,
        // and calibration aborts to the static gates. The region is
        // segmented AND passes the energy gate. Before the fix the
        // median was measured on speech, the utterance gate rose to ~3x
        // the speech RMS, and the first word was silently rejected —
        // the shared defect this fixture used to pin as-is.
        let samples = try Self.loadWavSamples("speech_during_calibration")

        var detector = UtteranceDetector(
            sampleRate: Self.sampleRate,
            frameFloor: 0.010,
            utteranceFloor: 0.008)

        var emitted: UtteranceDetector.Utterance?
        var i = 0
        while i + Self.frameLen <= samples.count {
            if let utterance = detector.process(Array(samples[i..<Self.frameLen + i])) {
                emitted = utterance
            }
            i += Self.frameLen
        }

        let utterance = try #require(emitted)
        // A window full of speech must abort calibration: the gates stay
        // at the static floors — the median may never be measured on speech.
        #expect(detector.currentEnergyThreshold == 0.008,
                "a window full of speech must abort calibration to the static gates")
        let rms = TranscriptFilter.rms(utterance.samples)
        #expect(rms >= detector.currentEnergyThreshold, """
            buffer RMS \(rms) should pass the static gate \
            \(detector.currentEnergyThreshold) — the first word must survive
            """)
    }

    @Test("speech over noise excludes speech from the median but still adapts")
    func speechOverNoiseRaisesGatesButExcludesSpeech() throws {
        // speech_over_noise_in_calibration: fan-level noise then speech
        // inside the window. The speech frames are excluded from the
        // median, the noise frames raise the gates, and the utterance
        // still clears the raised gate. A regression to "any speech →
        // skip calibration" would leave the gates static and let the
        // noise tail pass — this pins the exclusion mechanism itself.
        let samples = try Self.loadWavSamples("speech_over_noise_in_calibration")

        var detector = UtteranceDetector(
            sampleRate: Self.sampleRate,
            frameFloor: 0.010,
            utteranceFloor: 0.008)

        var emitted: UtteranceDetector.Utterance?
        var i = 0
        while i + Self.frameLen <= samples.count {
            if let utterance = detector.process(Array(samples[i..<Self.frameLen + i])) {
                emitted = utterance
            }
            i += Self.frameLen
        }

        let utterance = try #require(emitted)
        // The quiet (noise) frames raise the gate — speech exclusion must
        // not disable adaptation.
        #expect(detector.currentEnergyThreshold > 0.008,
                "the noise frames in the window must still raise the utterance gate")
        let rms = TranscriptFilter.rms(utterance.samples)
        #expect(rms >= detector.currentEnergyThreshold, """
            buffer RMS \(rms) should pass the raised gate \
            \(detector.currentEnergyThreshold)
            """)
    }

    // MARK: - Calibration median + abort boundary (mirror of the
    // speech-aware calibration block in tests/test_dictate.py)

    /// A frame of constant samples has RMS == |amplitude| — the cheapest
    /// input with a known energy. The amplitudes below are dyadic (exact in
    /// Float and Double alike), so the median/gate arithmetic is exact too.
    private static func constantFrame(_ amplitude: Float) -> [Float] {
        Array(repeating: amplitude, count: frameLen)
    }

    @Test("even count of non-uniform quiet frames: the two middle values are averaged")
    func evenQuietMedianAveragesTheTwoMiddleValues() throws {
        // Swift used to take the upper-middle element for even counts while
        // engine.py averages the two middle values — a divergence the golden
        // corpus cannot see (every fixture's quiet frames are uniform).
        // 10 frames at 1/128 + 10 at 1/64: the two middle values differ, and
        // the median must be their average (3/256), not the upper-middle
        // element (1/64).
        var detector = UtteranceDetector(
            sampleRate: Self.sampleRate,
            frameFloor: 0.010,
            utteranceFloor: 0.008)

        // 34 frames x 0.03s >= the 1s calibration window — 34 is the
        // engine's int(1.0 / 0.03) + 1, the frame count that triggers
        // calibration in production. 20 non-uniform quiet frames, then 14
        // speech frames (0.0625 >= calibrationSpeechFloor — excluded).
        for i in 0..<20 {
            _ = detector.process(Self.constantFrame(i % 2 == 0 ? 0.0078125 : 0.015625))
        }
        for _ in 20..<34 {
            _ = detector.process(Self.constantFrame(0.0625))
        }
        // Median = (1/128 + 1/64) / 2 = 3/256; utterance gate = 3/256 * 3.
        #expect(detector.currentEnergyThreshold == (0.0078125 + 0.015625) / 2 * 3.0)
    }

    @Test("exactly noiseMinimumSamples quiet frames still calibrates")
    func fiveQuietFramesStillCalibrate() throws {
        // The abort boundary is >= noiseMinimumSamples: the minimum count of
        // quiet frames must still adapt. A regression to a strict >
        // comparison — the mirror of engine.py's guard flipping < to <= —
        // would abort here and leave the static gates in force.
        var detector = UtteranceDetector(
            sampleRate: Self.sampleRate,
            frameFloor: 0.010,
            utteranceFloor: 0.008)

        for _ in 0..<5 {
            _ = detector.process(Self.constantFrame(0.015625))  // quiet
        }
        for _ in 5..<34 {
            _ = detector.process(Self.constantFrame(0.0625))    // speech
        }
        #expect(detector.currentEnergyThreshold == 0.015625 * 3.0,
                "5 quiet frames is exactly noiseMinimumSamples — must adapt")
    }

    @Test("one quiet frame short of noiseMinimumSamples aborts")
    func fourQuietFramesAbort() throws {
        // The boundary counterpart: 4 quiet frames is too few to trust, the
        // static gates stay in force for the session. Together with the
        // 5-quiet test this pins the comparison as >= (not >).
        var detector = UtteranceDetector(
            sampleRate: Self.sampleRate,
            frameFloor: 0.010,
            utteranceFloor: 0.008)

        for _ in 0..<4 {
            _ = detector.process(Self.constantFrame(0.015625))  // quiet
        }
        for _ in 4..<34 {
            _ = detector.process(Self.constantFrame(0.0625))    // speech
        }
        #expect(detector.currentEnergyThreshold == 0.008,
                "4 quiet frames < noiseMinimumSamples — calibration must abort")
    }
}
