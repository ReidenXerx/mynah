/// The states the UI reflects.
///
/// Raw values match the words the engines use for the same meaning, so logs
/// and any future IPC line up across implementations. `loading` is the
/// core's LOADING: a session that is starting while the model loads (the
/// old Swift app showed idle through those seconds; the engine makes the
/// state real).
enum DictationState: String, Sendable {
    case idle
    case loading
    case listening
    case transcribing

    /// Tint applied to the mynah bird in both the menu bar and the pill.
    /// RGBA values are carried over verbatim from `macos_indicator.py` and
    /// `macos_rumps.py` so the Swift app looks identical to what shipped;
    /// `loading` splits idle's grey toward the bird being half-awake.
    var tint: (r: Double, g: Double, b: Double, a: Double) {
        switch self {
        case .idle:         return (0.60, 0.60, 0.65, 1.00)
        case .loading:      return (0.45, 0.55, 0.75, 1.00)
        case .listening:    return (0.20, 0.80, 0.95, 1.00)
        case .transcribing: return (0.95, 0.70, 0.20, 1.00)
        }
    }

    var menuLabel: String {
        switch self {
        case .idle:         return "Idle"
        case .loading:      return "Loading…"
        case .listening:    return "Listening…"
        case .transcribing: return "Transcribing…"
        }
    }
}
