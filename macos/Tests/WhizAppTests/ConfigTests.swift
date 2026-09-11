import Testing
import Foundation
@testable import WhizApp

// swift-testing, not XCTest: XCTest ships only with a full Xcode install, while
// Testing.framework is present in Command Line Tools — which is how this app is
// built. Package.swift adds the CLT framework search path when it finds one, so
// `swift test` works without Xcode.

/// The config file is co-owned by the Swift app and the Python CLI, so these
/// tests are really compatibility tests against `whiz/config.py`.
@Suite("Flat TOML")
struct FlatTOMLTests {

    @Test("parses the scalar types the Python writer emits")
    func parsesScalars() {
        let values = FlatTOML.parse("""
        dictate_language = "ru"
        dictate_idle_timeout = 45.0
        ai_max_frames = 50
        dictate_vad = true
        dictate_idle_visible = false
        """)

        #expect(values["dictate_language"] == .string("ru"))
        #expect(values["dictate_idle_timeout"] == .double(45.0))
        #expect(values["ai_max_frames"] == .int(50))
        #expect(values["dictate_vad"] == .bool(true))
        #expect(values["dictate_idle_visible"] == .bool(false))
    }

    @Test("handles the escapes _emit_toml produces")
    func parsesEscapes() {
        let values = FlatTOML.parse(#"dictate_prompt = "say \"hi\" and \\ then stop""#)
        #expect(values["dictate_prompt"] == .string(#"say "hi" and \ then stop"#))
    }

    @Test("unescapes the control chars the Python writer now emits (C1)")
    func parsesControlCharEscapes() {
        // whiz/config.py `_escape_toml_string` escapes \n/\r/\t since the
        // wave-1 fix; previously a prompt containing a newline was written
        // as a raw byte, the line split in two, and the key was lost or
        // corrupted on BOTH sides (Swift's FlatTOML dropped the tail;
        // Python's tomllib read the file fine until Swift re-emitted it).
        let values = FlatTOML.parse(#"dictate_prompt = "line1\nline2\r\nline3\tend""#)
        #expect(values["dictate_prompt"] == .string("line1\nline2\r\nline3\tend"))

        // And the Swift writer's side of the contract: emit must re-escape.
        let round = FlatTOML.parse(FlatTOML.emit(["dictate_prompt": .string("a\nb\tc\rd")]))
        #expect(round["dictate_prompt"] == .string("a\nb\tc\rd"))
    }

    @Test("parses a document written with CRLF line endings (M7)")
    func parsesCRLFDocument() {
        let values = FlatTOML.parse("dictate_language = \"ru\"\r\ndictate_vad = true\r\n")
        #expect(values["dictate_language"] == .string("ru"))
        #expect(values["dictate_vad"] == .bool(true))
    }

    @Test("parses string arrays, including empty ones")
    func parsesArrays() {
        let values = FlatTOML.parse("""
        model_dirs = ["/one", "/two"]
        empty = []
        """)
        #expect(values["model_dirs"] == .stringArray(["/one", "/two"]))
        #expect(values["empty"] == .stringArray([]))
    }

    @Test("does not split arrays on commas inside quoted strings")
    func parsesCommaInString() {
        let values = FlatTOML.parse(#"model_dirs = ["/a,b", "/c"]"#)
        #expect(values["model_dirs"] == .stringArray(["/a,b", "/c"]))
    }

    @Test("keeps values that carry a trailing comment")
    func stripsTrailingComments() {
        // Regression: the RHS was passed to parseValue with the comment still
        // attached, so every numeric and bool key with a trailing comment
        // failed to parse and vanished. `WhizConfig.save()` then re-emitted
        // only what parsed, deleting those keys from disk — while Python's
        // tomllib read the same file fine.
        let values = FlatTOML.parse("""
        dictate_frame_energy = 0.010 # tuned for my mic
        dictate_vad = true  # silero
        ai_max_frames = 50 # cap
        dictate_vad_alt = true#no-space
        """)
        #expect(values["dictate_frame_energy"] == .double(0.010))
        #expect(values["dictate_vad"] == .bool(true))
        #expect(values["ai_max_frames"] == .int(50))
        #expect(values["dictate_vad_alt"] == .bool(true))
    }

    @Test("does not mistake a # inside a value for a comment")
    func keepsHashInsideValues() {
        #expect(FlatTOML.parse(#"p = "say #1 loudly""#)["p"] == .string("say #1 loudly"))
        #expect(FlatTOML.parse(#"d = ["/a#b", "/c"]"#)["d"] == .stringArray(["/a#b", "/c"]))
        #expect(FlatTOML.parse(#"p = "say #1" # comment"#)["p"] == .string("say #1"))
        #expect(FlatTOML.parse(#"p = "quote \" then #hash""#)
                == ["p": .string(#"quote " then #hash"#)])
    }

    @Test("a commented key survives a parse/emit round trip")
    func roundTripKeepsCommentedKeys() {
        // The end-to-end failure this guards: a hand-edited config with an
        // annotated value, saved by the Swift settings window.
        let document = """
        ai_model = "qwen3.5:9b"
        dictate_frame_energy = 0.010 # tuned
        dictate_vad = true # silero
        ocr_min_width = 1920
        """
        let parsed = FlatTOML.parse(document)
        let round = FlatTOML.parse(FlatTOML.emit(parsed))
        #expect(round.count == 4)
        #expect(round["dictate_frame_energy"] == .double(0.010))
        #expect(round["dictate_vad"] == .bool(true))
        #expect(round["ocr_min_width"] == .int(1920))
    }

    @Test("skips comments, blank lines and table headers")
    func skipsNonAssignments() {
        let values = FlatTOML.parse("""
        # a comment
        [section]

        dictate_language = "uk"
        """)
        #expect(values.count == 1)
        #expect(values["dictate_language"] == .string("uk"))
    }

    @Test("round-trips through emit unchanged")
    func roundTrips() {
        let original: [String: FlatTOML.Value] = [
            "dictate_language": .string("ru"),
            "dictate_prompt": .string(#"quote " and slash \"#),
            "dictate_idle_timeout": .double(45.0),
            "dictate_menu_bar": .bool(true),
            "model_dirs": .stringArray(["/one", "/two"]),
        ]
        #expect(FlatTOML.parse(FlatTOML.emit(original)) == original)
    }

    @Test("a multi-line prompt survives a full parse/emit round trip (C1)")
    func roundTripsMultiLinePrompt() {
        // The end-to-end failure: `whiz config set dictate_prompt='...'`
        // with an embedded newline used to corrupt the file for Swift
        // readers. The writer escapes, the parser unescapes, and a
        // double round trip must be stable.
        let original: [String: FlatTOML.Value] = [
            "dictate_prompt": .string("Отвечай кратко.\nПервая строка.\r\nВторая."),
        ]
        let emitted = FlatTOML.emit(original)
        let parsed = FlatTOML.parse(emitted)
        #expect(parsed == original)
        #expect(FlatTOML.parse(FlatTOML.emit(parsed)) == original)  // stable
    }
}

@Suite("WhizConfig", .serialized)
struct WhizConfigTests {

    @Test("defaults match whiz/config.py")
    func defaultsMatchPython() {
        let config = WhizConfig()
        #expect(config.language == "ru")
        #expect(config.hotkey == "<cmd>+<shift>+.")
        #expect(config.trigger == "toggle")
        #expect(config.idleTimeout == 45.0)
        #expect(config.autoStopSilence == 10.0)
        #expect(config.vad)
        #expect(config.showIndicator)
        #expect(config.menuBar)
        #expect(!config.idleVisible)
    }

    @Test("reads an int where a float is expected")
    func coercesIntToDouble() {
        // `whiz config set dictate_idle_timeout=0` writes a bare `0`.
        let config = WhizConfig.from(FlatTOML.parse("dictate_idle_timeout = 0"))
        #expect(config.idleTimeout == 0)
    }

    @Test("falls back to defaults for missing and mistyped keys")
    func toleratesBadValues() {
        let config = WhizConfig.from(FlatTOML.parse(#"dictate_vad = "yes""#))
        #expect(config.vad)  // default, not a crash
        #expect(config.language == "ru")
    }

    // MARK: - Load reporting (M10)

    /// `WhizConfig.path` honours `WHIZ_CONFIG_DIR`, exactly as `whiz/config.py`
    /// does — that is what isolates these tests from the real config and from
    /// each other (swift-testing runs them in parallel, so two tests must never
    /// share a directory). `setUnsetEnvironmentVariable` is unavailable on the
    /// macOS 13 SDK, hence the manual set/restore.
    private func withIsolatedConfigDir(_ body: () throws -> Void) rethrows {
        let previous = ProcessInfo.processInfo.environment["WHIZ_CONFIG_DIR"]
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("whiz-config-tests-\(UUID().uuidString)")
        setenv("WHIZ_CONFIG_DIR", dir.path, 1)
        defer {
            if let previous { setenv("WHIZ_CONFIG_DIR", previous, 1) }
            else { unsetenv("WHIZ_CONFIG_DIR") }
            try? FileManager.default.removeItem(at: dir)
        }
        try body()
    }

    @Test("an absent config file means defaults, with no error")
    func absentFileIsSilentDefaults() {
        withIsolatedConfigDir {
            let (config, error) = WhizConfig.loadReporting()
            #expect(config == WhizConfig())
            #expect(error == nil)
        }
    }

    @Test("an unreadable config file still yields defaults, but reports the failure")
    func unreadableFileIsReported() throws {
        try withIsolatedConfigDir {
            // A directory where the config file should be reads as EISDIR —
            // a real read failure, not "no file yet".
            try FileManager.default.createDirectory(
                at: WhizConfig.path, withIntermediateDirectories: true)
            let (config, error) = WhizConfig.loadReporting()
            #expect(config == WhizConfig())  // app still works on defaults
            guard case .unreadable(let path, _)? = error else {
                Issue.record("expected an unreadable error, got \(String(describing: error))")
                return
            }
            #expect(path == WhizConfig.path.path)
        }
    }

    @Test("a valid file loads cleanly with no error")
    func cleanReadHasNoError() throws {
        try withIsolatedConfigDir {
            try FileManager.default.createDirectory(
                at: WhizConfig.directory, withIntermediateDirectories: true)
            try FlatTOML.emit(["dictate_language": .string("uk"), "dictate_vad": .bool(false)])
                .write(to: WhizConfig.path, atomically: true, encoding: .utf8)
            let (config, error) = WhizConfig.loadReporting()
            #expect(error == nil)
            #expect(config.language == "uk")
            #expect(!config.vad)
        }
    }

    @Test("saving preserves keys owned by the Python side")
    func savePreservesForeignKeys() {
        var values = FlatTOML.parse("""
        ai_model = "qwen3:8b"
        model_dirs = ["/custom"]
        dictate_language = "ru"
        """)

        var config = WhizConfig.from(values)
        config.language = "uk"
        config.merged(into: &values)

        #expect(values["ai_model"] == .string("qwen3:8b"))
        #expect(values["model_dirs"] == .stringArray(["/custom"]))
        #expect(values["dictate_language"] == .string("uk"))
    }
}

@Suite("Hotkey parsing")
struct HotkeySpecTests {

    @Test("parses the shipping default")
    func parsesDefault() throws {
        let combo = try #require(HotkeySpec.parse("<cmd>+<shift>+."))
        #expect(combo.keyCode == 47)  // period
        #expect(combo.modifiers != 0)
    }

    @Test("parses a bare function key")
    func parsesFunctionKey() throws {
        let combo = try #require(HotkeySpec.parse("<f8>"))
        #expect(combo.keyCode == 100)
        #expect(combo.modifiers == 0)
    }

    @Test("accepts modifier aliases")
    func parsesAliases() throws {
        let long = try #require(HotkeySpec.parse("<command>+<control>+<option>+a"))
        let short = try #require(HotkeySpec.parse("<cmd>+<ctrl>+<alt>+a"))
        #expect(long.modifiers == short.modifiers)
        #expect(long.keyCode == short.keyCode)
    }

    @Test("rejects a spec with no key")
    func rejectsModifiersOnly() {
        #expect(HotkeySpec.parse("<cmd>+<shift>") == nil)
    }

    @Test("rejects an unknown key name")
    func rejectsUnknownKey() {
        #expect(HotkeySpec.parse("<cmd>+<nonsense>") == nil)
    }
}
