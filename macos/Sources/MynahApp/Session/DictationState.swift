import Foundation

/// The three states the UI reflects.
///
/// Raw values match the strings the Python engine passes to
/// `DictationIndicator.set_state`, so logs and any future IPC line up across
/// the two implementations.
enum DictationState: String, Sendable {
    case idle
    case listening
    case transcribing

    /// Tint applied to the W logo in both the menu bar and the pill.
    /// RGBA values are carried over verbatim from `macos_indicator.py` and
    /// `macos_rumps.py` so the Swift app looks identical to what shipped.
    var tint: (r: Double, g: Double, b: Double, a: Double) {
        switch self {
        case .idle:         return (0.60, 0.60, 0.65, 1.00)
        case .listening:    return (0.20, 0.80, 0.95, 1.00)
        case .transcribing: return (0.95, 0.70, 0.20, 1.00)
        }
    }

    var menuLabel: String {
        switch self {
        case .idle:         return "Idle"
        case .listening:    return "Listening…"
        case .transcribing: return "Transcribing…"
        }
    }
}
