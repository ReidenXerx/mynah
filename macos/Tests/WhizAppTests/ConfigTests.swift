import Testing
import Foundation
@testable import WhizApp

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
}

@Suite("WhizConfig")
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
