import Testing
import Foundation
@testable import WhizApp

// PINS NS-15 for the Swift resolver. `WhisperModel.preference` was previously
// a global unquantized-first batch, so a machine with only `tiny` +
// `large-v3-turbo-q8_0` on disk picked tiny — a useless-quality model
// outranking a large-class quantized one. The Python side pinned this
// (tests/test_models.py); until this suite nothing covered the Swift list.

@Suite("Whisper model preference")
struct WhisperModelTests {

    /// Strips any quantization suffix: ggml-large-v3-turbo-q8_0.bin ->
    /// ggml-large-v3-turbo.bin (handles q8_0/q5_0 and future -qN_M shapes).
    private func unquantizedBase(_ name: String) -> String {
        if let range = name.range(of: #"-q\d+_\d+\.bin$"#, options: .regularExpression) {
            return name.replacingCharacters(in: range, with: ".bin")
        }
        return name
    }

    @Test("every quantized variant ranks behind its unquantized class")
    func quantizedBehindUnquantizedBase() {
        for (index, name) in WhisperModel.preference.enumerated() {
            guard name.contains("-q") else { continue }
            let base = unquantizedBase(name)
            #expect(WhisperModel.preference.contains(base),
                    "\(name) has no unquantized \(base)")
            #expect(WhisperModel.preference.firstIndex(of: base)! < index,
                    "\(name) must rank behind \(base) (NS-15)")
        }
    }

    @Test("classes are contiguous, unquantized first, q8_0 before q5_0")
    func classGrouping() {
        let order = WhisperModel.preference
        // Consecutive-dedup yields the class sequence; anything not in the
        // expected classes would break contiguity/ordering here.
        let classes: [String] = order.map { name in
            guard let range = name.range(of: #"-q\d+_\d+\.bin$"#, options: .regularExpression) else {
                return String(name.dropFirst("ggml-".count).dropLast(".bin".count))
            }
            return String(name[name.startIndex..<range.lowerBound]
                .dropFirst("ggml-".count))
        }
        var seen: [String] = []
        for cls in classes {
            if seen.last != cls {
                #expect(!seen.contains(cls), "\(cls) class is not contiguous")
                seen.append(cls)
            }
        }
        #expect(seen == ["large-v3-turbo", "large-v3", "medium"])
        // Within the turbo class, q8_0 (higher quality) ranks ahead of q5_0.
        let q8 = order.firstIndex(of: "ggml-large-v3-turbo-q8_0.bin")
        let q5 = order.firstIndex(of: "ggml-large-v3-turbo-q5_0.bin")
        #expect(q8! < q5!)
    }

    @Test("turbo-q8_0 outranks everything after its class — tiny must never win")
    func turboQ8BeatsSmallerClasses() {
        // The review repro: a disk holding only a small-class unquantized
        // model plus large-v3-turbo-q8_0 must resolve turbo-q8_0.
        #expect(WhisperModel.preference.firstIndex(of: "ggml-large-v3-turbo-q8_0.bin")!
                < WhisperModel.preference.firstIndex(of: "ggml-medium.bin")!)
    }

    @Test("tiny is excluded from the preference list")
    func tinyExcluded() {
        for name in WhisperModel.preference {
            #expect(!name.contains("tiny"),
                    "tiny is useless quality — never auto-picked (NS-15)")
        }
    }

    @Test("resolve prefers unquantized turbo when both variants exist")
    func resolvePrefersUnquantized() {
        // resolve() walks search directories; point it at a temp dir holding
        // both turbo variants and confirm the unquantized file wins.
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("whiz-model-tests-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let unquantized = dir.appendingPathComponent("ggml-large-v3-turbo.bin")
        let quantized = dir.appendingPathComponent("ggml-large-v3-turbo-q8_0.bin")
        FileManager.default.createFile(atPath: unquantized.path, contents: Data())
        FileManager.default.createFile(atPath: quantized.path, contents: Data())
        defer { try? FileManager.default.removeItem(at: dir) }

        // Inject the temp dir through the parameter — mutating static state
        // would race under swift-testing's parallel runs.
        let resolved = WhisperModel.resolve(configured: "", searchDirs: [dir])
        #expect(resolved?.lastPathComponent == "ggml-large-v3-turbo.bin")
    }
}