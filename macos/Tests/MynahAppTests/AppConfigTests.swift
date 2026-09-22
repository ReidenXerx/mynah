// The app's half of the settings path (Phase 5).
//
// The engine owns config.toml and validates every write; what lives here is
// the adapter between it and SwiftUI — `AppConfig(json:)` reading
// `mynah_config_json`, and `changes(toward:)` producing the (key, JSON)
// pairs `mynah_config_set` takes. Both had bugs that shipped: the diff sent
// the OLD value instead of the new one, and string values arrived without
// their quotes, so every string setting snapped back in the UI. The core
// suite cannot see either — they are on this side of the boundary.

import Foundation
import Testing

@testable import MynahApp

@Suite("AppConfig")
struct AppConfigTests {

    @Test("reads every key the engine emits")
    func readsEveryKey() throws {
        let json = """
        {"model":"ggml-small.bin","language":"uk","prompt":"hi","hotkey":"<f8>",
         "trigger":"ptt","transcription_mode":"on_stop","injector":"wtype",
         "vad":false,"gpu":true,"show_indicator":false,"idle_visible":true,
         "idle_timeout":0,"auto_stop_silence":7.5,"frame_energy":0.02,
         "min_energy":0.009,"min_utterance":0.3}
        """
        let config = AppConfig(json: json)
        #expect(config.model == "ggml-small.bin")
        #expect(config.language == "uk")
        #expect(config.prompt == "hi")
        #expect(config.hotkey == "<f8>")
        #expect(config.trigger == "ptt")
        #expect(config.transcriptionMode == "on_stop")
        #expect(config.injector == "wtype")
        #expect(config.vad == false)
        #expect(config.gpu == true)
        #expect(config.showIndicator == false)
        #expect(config.idleVisible == true)
        // An integer where a double is expected: the engine prints whole
        // numbers without a decimal point.
        #expect(config.idleTimeout == 0)
        #expect(config.autoStopSilence == 7.5)
        #expect(config.frameEnergy == 0.02)
        #expect(config.minEnergy == 0.009)
        #expect(config.minUtterance == 0.3)
    }

    @Test("missing keys, wrong types and malformed JSON fall back to defaults")
    func tolerance() {
        let defaults = AppConfig()
        #expect(AppConfig(json: "{}") == defaults)
        #expect(AppConfig(json: "not json at all") == defaults)
        #expect(AppConfig(json: "") == defaults)
        // Truncated JSON — the exact break that made every setting read as a
        // default once (the writer omitted the closing brace).
        #expect(AppConfig(json: "{\"language\":\"uk\"") == defaults)
        // A value of the wrong type is ignored, key by key.
        let mixed = AppConfig(json: "{\"vad\":\"yes\",\"language\":\"uk\"}")
        #expect(mixed.vad == defaults.vad)
        #expect(mixed.language == "uk")
    }

    @Test("a change carries the NEW value, and only the keys that differ")
    func changesCarryTheNewValue() throws {
        var updated = AppConfig()
        updated.language = "uk"
        updated.vad = false

        let changes = AppConfig().changes(toward: updated)
        #expect(changes.count == 2)
        let byKey = Dictionary(uniqueKeysWithValues: changes.map { ($0.key, $0.json) })
        // WITH the quotes: the engine's JSON parser refuses a bare word, and
        // the setting snapped back to its old value in the UI.
        #expect(byKey["language"] == "\"uk\"")
        #expect(byKey["vad"] == "false")
        #expect(AppConfig().changes(toward: AppConfig()).isEmpty)
    }

    @Test("every emitted value is valid JSON of the right kind")
    func valuesAreValidJSON() throws {
        var updated = AppConfig()
        updated.model = "ggml-large-v3-turbo.bin"
        updated.prompt = #"quotes " backslash \ and a newline"# + "\n"
        updated.trigger = "ptt"
        updated.transcriptionMode = "on_stop"
        updated.gpu = true
        updated.idleTimeout = 0
        updated.autoStopSilence = 12.5
        updated.frameEnergy = 0.0125

        for change in AppConfig().changes(toward: updated) {
            let data = Data(change.json.utf8)
            let parsed = try JSONSerialization.jsonObject(
                with: data, options: [.fragmentsAllowed])
            switch change.key {
            case "model", "prompt", "trigger", "transcription_mode":
                #expect(parsed is String, "\(change.key) must encode as a JSON string")
            case "gpu":
                #expect(parsed is NSNumber) // JSON true
            default:
                #expect(parsed is NSNumber, "\(change.key) must encode as a JSON number")
            }
        }
    }

    @Test("a whole number keeps its decimal point, so it stays a float")
    func wholeNumbersStayFloats() {
        var updated = AppConfig()
        updated.idleTimeout = 45
        let change = try? #require(AppConfig(idleTimeoutOverride: 0).changes(toward: updated).first)
        #expect(change?.json == "45.0")
    }

    @Test("a round trip through the wire format preserves the settings")
    func roundTrip() throws {
        var updated = AppConfig()
        updated.language = "en"
        updated.transcriptionMode = "on_stop"
        updated.minUtterance = 0.42
        updated.prompt = "say \"this\""

        // Rebuild the engine's JSON object from the changes and read it back,
        // the way the app does after a write.
        var object: [String: Any] = [:]
        for change in AppConfig().changes(toward: updated) {
            object[change.key] = try JSONSerialization.jsonObject(
                with: Data(change.json.utf8), options: [.fragmentsAllowed])
        }
        let data = try JSONSerialization.data(withJSONObject: object)
        let parsed = AppConfig(json: String(decoding: data, as: UTF8.self))
        #expect(parsed.language == "en")
        #expect(parsed.transcriptionMode == "on_stop")
        #expect(parsed.minUtterance == 0.42)
        #expect(parsed.prompt == "say \"this\"")
    }
}

private extension AppConfig {
    /// A config whose `idleTimeout` differs, so a single change is produced.
    init(idleTimeoutOverride: Double) {
        self.init()
        idleTimeout = idleTimeoutOverride
    }
}
