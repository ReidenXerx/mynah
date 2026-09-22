import Foundation

/// The app's typed mirror of the engine's config.
///
/// Phase 5: the CORE owns `~/.config/mynah/config.toml` — parsing, defaults,
/// validation and the read-modify-write save (P5). This struct is what the
/// settings UI binds to; it is built from `mynah_config_json` and written
/// back through `mynah_config_set` per changed key. The wire keys stay
/// snake_case, exactly the TOML keys, so the adapter is a dictionary lookup
/// with no mapping table to drift.
///
/// Defaults mirror the engine's, which mirror `mynah/config.py` — pinned by
/// the tuning contract on the core. If they drift, the "Restore Defaults"
/// button here would restore values the engine does not consider default:
/// keep them in step.
struct AppConfig: Equatable {
    var model = ""
    var language = "ru"
    var prompt = ""
    var hotkey = "<cmd>+<shift>+."
    var trigger = "toggle"
    var injector = ""
    var vad = true
    var gpu = false
    var showIndicator = true
    var idleVisible = false
    var idleTimeout = 45.0
    var autoStopSilence = 10.0
    var frameEnergy = 0.010
    var minEnergy = 0.008
    var minUtterance = 0.25

    /// The engine's defaults.
    init() {}

    /// Parse `mynah_config_json`'s output. Unknown keys are ignored, missing
    /// keys fall back to the defaults — the same tolerance the engine's own
    /// loader has.
    init(json: String) {
        self.init()
        guard let data = json.data(using: .utf8),
              let values = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return }
        if let v = values["model"] as? String { model = v }
        if let v = values["language"] as? String { language = v }
        if let v = values["prompt"] as? String { prompt = v }
        if let v = values["hotkey"] as? String { hotkey = v }
        if let v = values["trigger"] as? String { trigger = v }
        if let v = values["injector"] as? String { injector = v }
        if let v = values["vad"] as? Bool { vad = v }
        if let v = values["gpu"] as? Bool { gpu = v }
        if let v = values["show_indicator"] as? Bool { showIndicator = v }
        if let v = values["idle_visible"] as? Bool { idleVisible = v }
        if let v = values["idle_timeout"] as? Double { idleTimeout = v }
        if let v = values["auto_stop_silence"] as? Double { autoStopSilence = v }
        if let v = values["frame_energy"] as? Double { frameEnergy = v }
        if let v = values["min_energy"] as? Double { minEnergy = v }
        if let v = values["min_utterance"] as? Double { minUtterance = v }
    }
}

/// The wire format: (key, JSON encoding) for every field that differs.
extension AppConfig {
    /// The settings this config would change relative to `other`, each as
    /// (snake_case key, JSON-encoded value) — the shape `mynah_config_set`
    /// takes. Used by the controller to write back only what changed.
    func changes(toward other: AppConfig) -> [(key: String, json: String)] {
        var changes: [(key: String, json: String)] = []
        func add(_ key: String, _ mine: String, _ theirs: String) {
            if mine != theirs { changes.append((key: key, json: mine.jsonEncoded)) }
        }
        func add(_ key: String, _ mine: Bool, _ theirs: Bool) {
            if mine != theirs { changes.append((key: key, json: mine ? "true" : "false")) }
        }
        func add(_ key: String, _ mine: Double, _ theirs: Double) {
            if mine != theirs {
                // Keep the decimal point so the value survives as a float:
                // the engine's TOML reader accepts either shape, but the
                // Python side wrote floats with one too.
                changes.append((key: key,
                                json: mine == mine.rounded() && abs(mine) < 1e15
                                    ? String(format: "%.1f", mine)
                                    : String(mine)))
            }
        }

        add("model", model, other.model)
        add("language", language, other.language)
        add("prompt", prompt, other.prompt)
        add("hotkey", hotkey, other.hotkey)
        add("trigger", trigger, other.trigger)
        add("injector", injector, other.injector)
        add("vad", vad, other.vad)
        add("gpu", gpu, other.gpu)
        add("show_indicator", showIndicator, other.showIndicator)
        add("idle_visible", idleVisible, other.idleVisible)
        add("idle_timeout", idleTimeout, other.idleTimeout)
        add("auto_stop_silence", autoStopSilence, other.autoStopSilence)
        add("frame_energy", frameEnergy, other.frameEnergy)
        add("min_energy", minEnergy, other.minEnergy)
        add("min_utterance", minUtterance, other.minUtterance)
        return changes
    }
}

private extension String {
    /// This string as a JSON scalar value.
    var jsonEncoded: String {
        guard let data = try? JSONSerialization.data(withJSONObject: [self]),
              let array = try? JSONSerialization.jsonObject(with: data) as? [String],
              let encoded = array.first else { return "\"\"" }
        return encoded
    }
}