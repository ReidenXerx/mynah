import Foundation

/// Typed access to `~/.config/whiz/config.toml`, shared with the Python CLI.
///
/// The file is a single flat table owned by *both* sides: Python writes
/// pipeline keys (`ai_model`, `model_dirs`, …), Swift writes dictation keys
/// (`dictate_*`). So saving is always read-modify-write — `save()` reparses the
/// file and overwrites only the dictation keys, leaving every other key byte-
/// identical. Clobbering the user's `whiz analyze` settings by rewriting the
/// whole file from a Swift-side struct would be a nasty regression, and it is
/// the specific thing `WhizConfigTests.testSavePreservesForeignKeys` guards.
///
/// Defaults mirror `whiz/config.py`. Keep them in sync — if they drift, the
/// same config file means two different things depending on which binary reads
/// it.
struct WhizConfig {

    // MARK: - Dictation keys (Swift-owned)

    var model: String = ""
    var language: String = "ru"
    var prompt: String = ""
    var idleTimeout: Double = 45.0
    var hotkey: String = "<cmd>+<shift>+."
    var trigger: String = "toggle"
    var vad: Bool = true
    var autoStopSilence: Double = 10.0
    var showIndicator: Bool = true
    var idleVisible: Bool = false
    var menuBar: Bool = true

    // Microphone sensitivity. Defaults match `whiz/config.py` — see the comment
    // there for why they were lowered from the original 0.03 / 0.025.
    var frameEnergy: Double = 0.010
    var minEnergy: Double = 0.008
    var minUtterance: Double = 0.25

    /// Mapping from struct property to TOML key. Single source of truth for
    /// both load and save so the two can't drift.
    private static let keys = (
        model: "dictate_model",
        language: "dictate_language",
        prompt: "dictate_prompt",
        idleTimeout: "dictate_idle_timeout",
        hotkey: "dictate_hotkey",
        trigger: "dictate_trigger",
        vad: "dictate_vad",
        autoStopSilence: "dictate_auto_stop_silence",
        showIndicator: "dictate_show_indicator",
        idleVisible: "dictate_idle_visible",
        menuBar: "dictate_menu_bar",
        frameEnergy: "dictate_frame_energy",
        minEnergy: "dictate_min_energy",
        minUtterance: "dictate_min_utterance"
    )

    // MARK: - Location

    /// Honours `WHIZ_CONFIG_DIR` exactly as `whiz/config.py` does, so tests and
    /// alternate profiles work identically from either side.
    static var directory: URL {
        if let override = ProcessInfo.processInfo.environment["WHIZ_CONFIG_DIR"], !override.isEmpty {
            return URL(fileURLWithPath: (override as NSString).expandingTildeInPath)
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".config/whiz")
    }

    static var path: URL { directory.appendingPathComponent("config.toml") }

    // MARK: - Load

    /// Load from disk, falling back to defaults for anything absent or of the
    /// wrong type. Never throws — a corrupt config should start the app with
    /// defaults, not prevent it from launching.
    static func load() -> WhizConfig {
        guard let text = try? String(contentsOf: path, encoding: .utf8) else {
            return WhizConfig()
        }
        return from(FlatTOML.parse(text))
    }

    static func from(_ values: [String: FlatTOML.Value]) -> WhizConfig {
        var c = WhizConfig()
        if case .string(let v)? = values[keys.model] { c.model = v }
        if case .string(let v)? = values[keys.language] { c.language = v }
        if case .string(let v)? = values[keys.prompt] { c.prompt = v }
        if let v = number(values[keys.idleTimeout]) { c.idleTimeout = v }
        if case .string(let v)? = values[keys.hotkey] { c.hotkey = v }
        if case .string(let v)? = values[keys.trigger] { c.trigger = v }
        if case .bool(let v)? = values[keys.vad] { c.vad = v }
        if let v = number(values[keys.autoStopSilence]) { c.autoStopSilence = v }
        if case .bool(let v)? = values[keys.showIndicator] { c.showIndicator = v }
        if case .bool(let v)? = values[keys.idleVisible] { c.idleVisible = v }
        if case .bool(let v)? = values[keys.menuBar] { c.menuBar = v }
        if let v = number(values[keys.frameEnergy]) { c.frameEnergy = v }
        if let v = number(values[keys.minEnergy]) { c.minEnergy = v }
        if let v = number(values[keys.minUtterance]) { c.minUtterance = v }
        return c
    }

    /// TOML has no float/int coercion, but Python writes `45.0` as a float and
    /// `0` as an int for the same field, so accept either.
    private static func number(_ value: FlatTOML.Value?) -> Double? {
        switch value {
        case .double(let d): return d
        case .int(let i): return Double(i)
        default: return nil
        }
    }

    // MARK: - Save

    /// Merge this struct's dictation keys into the on-disk config, preserving
    /// every key we don't own.
    func save() throws {
        var values: [String: FlatTOML.Value]
        if let text = try? String(contentsOf: Self.path, encoding: .utf8) {
            values = FlatTOML.parse(text)
        } else {
            values = [:]
        }
        merged(into: &values)
        try FileManager.default.createDirectory(
            at: Self.directory, withIntermediateDirectories: true)
        try FlatTOML.emit(values).write(to: Self.path, atomically: true, encoding: .utf8)
    }

    func merged(into values: inout [String: FlatTOML.Value]) {
        let k = Self.keys
        values[k.model] = .string(model)
        values[k.language] = .string(language)
        values[k.prompt] = .string(prompt)
        values[k.idleTimeout] = .double(idleTimeout)
        values[k.hotkey] = .string(hotkey)
        values[k.trigger] = .string(trigger)
        values[k.vad] = .bool(vad)
        values[k.autoStopSilence] = .double(autoStopSilence)
        values[k.showIndicator] = .bool(showIndicator)
        values[k.idleVisible] = .bool(idleVisible)
        values[k.menuBar] = .bool(menuBar)
        values[k.frameEnergy] = .double(frameEnergy)
        values[k.minEnergy] = .double(minEnergy)
        values[k.minUtterance] = .double(minUtterance)
    }
}
