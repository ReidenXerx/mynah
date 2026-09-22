import AppKit
import Combine
import CMynah
import Foundation

/// Bridges the SwiftUI app to the C++ core.
///
/// Phase 5: everything that used to live in this file and in
/// `UtteranceDetector` / `TranscriptFilter` / `WhisperEngine` / `SileroVAD` /
/// `WhisperModel` is now `libmynah` behind the C API. What remains here is
/// the adapter's three jobs:
///
///   1. publish the engine's events to SwiftUI — they arrive on engine
///      threads and hop to the main actor here;
///   2. own the capture (`AudioCapture` → `mynah_push_audio`, the only call
///      the engine allows from a real-time thread);
///   3. surface the config for the settings UI, writing through
///      `mynah_config_set` (the core saves; the app never touches the file).
///
/// The engine's own semantics replace the Swift ones they supersede: the
/// cold model load is asynchronous inside the engine (LOADING arrives as a
/// state event), stop returns at once and drains as events, auto-stop and
/// the idle unload are the core's. The old Swift counterparts are gone.
@MainActor
final class SessionController: ObservableObject {

    @Published private(set) var state: DictationState = .idle
    @Published private(set) var isSessionActive = false

    /// Live mic amplitude in 0...1, feeding the pill's waveform bars. The
    /// engine meters inside its own frame loop; the tap delivers raw samples.
    @Published private(set) var level: Double = 0.0

    /// Surfaced in the menu so failures are visible rather than silent.
    @Published private(set) var lastError: String?

    /// Live Accessibility state.
    ///
    /// Polled rather than read once: the menu used to capture this into `@State`
    /// at first render, so granting the permission never updated the display —
    /// it read "not granted" forever, which looked exactly like the grant having
    /// failed. TCC sends no notification, so polling is the only option.
    @Published private(set) var isAccessibilityTrusted = Permissions.isAccessibilityTrusted

    @Published var config: AppConfig

    /// True when `vad` is on but the Silero model could not be loaded: the
    /// toggle reading "on" must not silently mean "loudness-only rejection"
    /// (M5). The engine reports the degradation as a `vad_degraded` problem.
    @Published private(set) var isVADDegraded = false

    private let capture = AudioCapture()

    /// The engine pointer lives in a Sendable box so the nonisolated deinit
    /// can read it without touching main-actor state. The main-actor code
    /// reads it through `engine` (guarded by the actor).
    private let engineBox = EngineBox()
    private var engine: OpaquePointer? { engineBox.value }
    private let eventSink = EventSink()

    /// True while a session is live or still loading — the engine's
    /// LOADING state is what the old `startTask` bookkeeping tracked by hand.
    var isEngaged: Bool { state != .idle }

    init() {
        config = AppConfig()
        var error: UnsafeMutablePointer<CChar>? = nil
        // config = NULL: the default path, with the one-time whiz import.
        // The sink travels as the callback's user pointer — the engine can
        // deliver the moment it is created — and the controller attaches
        // itself to the sink right after, before any session can start.
        engineBox.value = mynah_create(nil, Self.eventTrampoline,
                                       Unmanaged.passUnretained(eventSink).toOpaque(),
                                       &error)
        if let error {
            lastError = String(cString: error)
            free(error)
        }
        reloadConfigFromEngine()
        eventSink.attach(self)
    }

    deinit {
        // Normally the engine is destroyed by `shutdownBlocking` at quit; a
        // controller dropped without that still has to free it. Plain C call,
        // safe from the nonisolated deinit through the Sendable box.
        if let engine = engineBox.value { mynah_destroy(engine) }
    }

    // MARK: - Trigger

    func toggleSession() {
        guard let engine else { return }
        mynah_toggle(engine)
    }

    /// Starts a session. The model load happens inside the engine — the
    /// LOADING state arrives as an event, and a second press of the hotkey
    /// cancels the whole thing (`mynah_stop` on a pending start is its
    /// cancel). The microphone is requested in parallel: the old app asked
    /// before starting capture because granting mid-session yields silence
    /// for that whole session.
    func startSession() {
        guard let engine, !isEngaged else { return }
        lastError = nil
        mynah_start(engine)

        Task { [weak self] in
            guard let self else { return }
            let granted = await Permissions.requestMicrophone()
            guard isEngaged else { return } // cancelled during the prompt or the load
            if granted {
                do {
                    try startCapture()
                } catch {
                    Log.audio.error(
                        "capture failed: \(error.localizedDescription, privacy: .public)")
                    lastError = error.localizedDescription
                    endSession()
                }
            } else {
                Log.session.error("microphone permission denied")
                lastError = "Microphone access is required. Enable mynah in "
                    + "System Settings → Privacy & Security → Microphone."
                endSession()
            }
        }
    }

    func endSession() {
        guard let engine else { return }
        // Returns at once; remaining utterances arrive as events and the
        // state falls back to idle as they complete.
        mynah_stop(engine)
        isSessionActive = false
        level = 0
    }

    // MARK: - Config

    /// The config file's path, for the "Open Config File" menu item.
    var configFilePath: String {
        guard let engine, let path = mynah_config_path(engine) else {
            return NSString(string: "~/.config/mynah/config.toml").expandingTildeInPath
        }
        let value = String(cString: path)
        free(path)
        return value
    }

    /// Mutate settings, persist them through the core, and republish what
    /// the engine accepted. Saving is the core's read-modify-write, so keys
    /// owned by any other writer survive. Most settings take effect on the
    /// next session; the ones that cannot are marked in the UI.
    func updateConfig(_ mutate: (inout AppConfig) -> Void) {
        var updated = config
        mutate(&updated)
        guard updated != config else { return }
        guard let engine else { return }

        // One key at a time: mynah_config_set validates each value against
        // the engine's own rules, so a mistyped bool can never reach the
        // file — and a refused key leaves the rest applied.
        let previous = config
        for change in previous.changes(toward: updated) {
            let code = change.json.withCString { json in
                mynah_config_set(engine, change.key, json)
            }
            if code != 0 {
                Log.session.error(
                    "the engine refused \(change.key, privacy: .public) — not saved")
            }
        }
        reloadConfigFromEngine()

        // A flipped VAD toggle applies at the next session start; the
        // engine reports a degradation (if any) as a problem event then.
        if updated.vad != previous.vad { isVADDegraded = false }
    }

    /// Reload the config from the engine into the published struct — the
    /// engine is the authority on what the file now says.
    private func reloadConfigFromEngine() {
        guard let engine, let json = mynah_config_json(engine) else { return }
        config = AppConfig(json: String(cString: json))
        free(json)
    }

    // MARK: - Model surface for the settings window

    /// The model the engine would load for the configured value, or nil —
    /// which is what Settings turns into a download (M4: turbo, or a
    /// `no_model` problem that Settings turns into a download).
    var installedModel: String? {
        guard let path = mynah_find_model(config.model) else { return nil }
        defer { free(path) }
        return URL(fileURLWithPath: String(cString: path)).lastPathComponent
    }

    /// Whether the Silero VAD model is on disk, resolved through the same
    /// directories the engine's session loader uses.
    var hasVADModel: Bool {
        guard let path = mynah_find_vad() else { return false }
        defer { free(path) }
        return true
    }

    // MARK: - Capture

    private func startCapture() throws {
        guard let engine else { return }
        // The engine pointer is an identity, not state: it never changes and
        // outlives the tap. An Int64 travels through @Sendable closures;
        // pointers do not.
        let target = Int64(bitPattern: UInt64(UInt(bitPattern: engine)))
        // The tap runs on the audio thread; `mynah_push_audio` is the one
        // call the engine allows from a real-time thread (never blocks,
        // never allocates). Samples pushed before the engine arms are
        // dropped by the ring, so arming slightly early is free.
        try capture.start { samples in
            mynah_push_audio(OpaquePointer(bitPattern: UInt(truncatingIfNeeded: target)),
                             samples, samples.count)
        } onConfigurationChange: { [weak self] in
            Task { @MainActor in self?.handleCaptureConfigurationChange() }
        }
    }

    private func handleCaptureConfigurationChange() {
        guard isSessionActive else { return }
        do {
            try capture.restart()
            Log.audio.notice("capture stream rebuilt after a device change")
        } catch {
            Log.audio.error(
                "capture restart failed: \(error.localizedDescription, privacy: .public)")
            lastError = "Microphone changed and capture could not restart: "
                + error.localizedDescription
            endSession()
        }
    }

    // MARK: - Permissions and errors

    /// Re-read permission state. Called on a timer from `AppDelegate`.
    func refreshPermissions() {
        let trusted = Permissions.isAccessibilityTrusted
        guard trusted != isAccessibilityTrusted else { return }
        isAccessibilityTrusted = trusted
        Log.ui.notice("accessibility trust changed: \(trusted, privacy: .public)")
        // Clear the stale complaint as soon as the grant lands, so the menu does
        // not keep accusing the user of something they have already done.
        if trusted, lastError?.contains("Accessibility") == true { lastError = nil }
    }

    /// Surface a failure raised outside the controller (e.g. hotkey registration).
    func reportError(_ message: String) {
        lastError = message
    }

    // MARK: - Shutdown

    /// Free the engine before the process exits, blocking for it.
    ///
    /// The destroy contract is what makes quitting with a model loaded exit
    /// 0: it joins the engine's workers and frees the whisper and VAD
    /// contexts before returning — ggml aborts at exit if a Metal context is
    /// still alive (the old `shutdownBlocking` existed for the same reason).
    ///
    /// Blocking the main thread is acceptable here; the app is terminating.
    func shutdownBlocking() {
        endSession()
        capture.stop()
        if let engine {
            mynah_destroy(engine)
            engineBox.value = nil
        }
    }

    // MARK: - Events from the core

    /// The event handler, on the main actor. The engine delivers on its own
    /// threads; `EventSink` copies each event out of the callback's memory
    /// and hops here.
    private func handle(_ event: EngineEventData) {
        switch event.kind {
        case .state:
            state = event.state
            isSessionActive = event.state != .idle
            if event.state == .idle { level = 0 }
        case .level:
            level = event.level
        case .text:
            // The last line of defence (the hallucination filter) already ran
            // in the core; this is where the text reaches the keyboard.
            TextInjector.type(event.text)
        case .problem:
            if event.problemCode == "vad_degraded" { isVADDegraded = true }
            Log.session.error(
                "\(event.problemCode, privacy: .public): \(event.problemMessage, privacy: .public)")
            lastError = event.problemMessage
        case .model:
            Log.stt.notice(
                "model \(event.modelStatus, privacy: .public) \(event.modelName, privacy: .public)")
            if event.modelStatus == "loaded" {
                isVADDegraded = false
                if lastError?.contains("model") == true { lastError = nil }
            }
        }
    }

    nonisolated func receive(_ event: EngineEventData) {
        Task { @MainActor in handle(event) }
    }

    /// The C trampoline. Static, `@convention(c)`, nonisolated — no captures,
    /// the sink comes through the user pointer. `mynah_event` is opaque (no
    /// public layout), so the event travels as an OpaquePointer and every
    /// read goes through the accessor functions.
    fileprivate nonisolated static let eventTrampoline: mynah_event_fn = { event, user in
        guard let event, let user else { return }
        EventSink.deliver(event, to: user)
    }

    fileprivate nonisolated static func dictationState(_ state: mynah_state) -> DictationState {
        switch state {
        case MYNAH_IDLE: return .idle
        case MYNAH_LOADING: return .loading
        case MYNAH_LISTENING: return .listening
        case MYNAH_TRANSCRIBING: return .transcribing
        default: return .idle
        }
    }
}

#if DEBUG
extension SessionController {
    /// Builds a controller in a chosen visual state, for SwiftUI previews.
    ///
    /// `state` and `level` are `private(set)` — only the session lifecycle
    /// moves them. Previews are the one legitimate exception, and confining
    /// the escape hatch to `#if DEBUG` keeps it out of shipping builds. The
    /// extension lives in this file so the `private(set)` setters are
    /// reachable.
    static func preview(
        state: DictationState = .listening,
        level: Double = 0.55,
        error: String? = nil
    ) -> SessionController {
        let controller = SessionController()
        controller.state = state
        controller.level = level
        controller.lastError = error
        return controller
    }
}
#endif

/// The engine pointer as a Sendable value the nonisolated deinit can read
/// without touching main-actor state.
final class EngineBox: @unchecked Sendable {
    var value: OpaquePointer?
}

/// The engine's events, copied out of the callback's `mynah_event` (valid
/// only for the callback's duration) before hopping to the main actor.
///
/// All fields are value types, so the copy is Sendable end to end.
struct EngineEventData: Sendable {
    enum Kind: Sendable {
        case state
        case level
        case text
        case problem
        case model
    }

    var kind: Kind
    var state: DictationState = .idle
    var level: Double = 0
    var text: String = ""
    var problemCode: String = ""
    var problemMessage: String = ""
    var modelStatus: String = ""
    var modelName: String = ""
}

/// Holds the Swift-side event routing for the C callback's lifetime. The
/// engine gets the sink as its callback's user pointer at create time, and
/// the controller attaches itself right after init — events cannot fire
/// before a session starts, so the nil window is unreachable in practice,
/// and a nil attachment degrades to a no-op rather than a crash.
final class EventSink: @unchecked Sendable {
    private weak var controller: SessionController?

    init() {}

    func attach(_ controller: SessionController) {
        self.controller = controller
    }

    fileprivate static func deliver(_ event: OpaquePointer, to user: UnsafeMutableRawPointer) {
        let sink = Unmanaged<EventSink>.fromOpaque(user).takeUnretainedValue()
        var data = EngineEventData(kind: .state)
        switch mynah_event_get_kind(event) {
        case MYNAH_EVENT_STATE:
            data = EngineEventData(kind: .state,
                                   state: SessionController.dictationState(
                                       mynah_event_state(event)))
        case MYNAH_EVENT_LEVEL:
            data = EngineEventData(kind: .level, level: Double(mynah_event_level(event)))
        case MYNAH_EVENT_TEXT:
            data = EngineEventData(kind: .text, text: stringFrom(event) { mynah_event_text($0) })
        case MYNAH_EVENT_PROBLEM:
            data = EngineEventData(kind: .problem,
                                   problemCode: stringFrom(event) { mynah_event_problem_code($0) },
                                   problemMessage: stringFrom(event) {
                                       mynah_event_problem_message($0)
                                   })
        case MYNAH_EVENT_MODEL:
            data = EngineEventData(kind: .model,
                                   modelStatus: modelStatusWord(mynah_event_model_status(event)),
                                   modelName: stringFrom(event) { mynah_event_model_name($0) })
        default:
            return
        }
        sink.controller?.receive(data)
    }

    private static func stringFrom(_ event: OpaquePointer,
                                   _ accessor: (OpaquePointer) -> UnsafePointer<CChar>?)
        -> String
    {
        guard let raw = accessor(event) else { return "" }
        return String(cString: raw)
    }

    private static func modelStatusWord(_ status: mynah_model_status) -> String {
        switch status {
        case MYNAH_MODEL_LOADING: return "loading"
        case MYNAH_MODEL_LOADED: return "loaded"
        case MYNAH_MODEL_UNLOADED: return "unloaded"
        default: return "unloaded"
        }
    }
}