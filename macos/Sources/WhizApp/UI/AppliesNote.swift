import SwiftUI

/// When a setting actually takes effect.
///
/// Worth stating per-setting rather than warning globally, because the three
/// cases genuinely differ and guessing wrong is the annoying part: whiz re-reads
/// its config at the start of every dictation, so almost everything applies
/// without a restart — but the indicator panel is built once at launch, so
/// toggling it looks broken until you quit and reopen.
enum AppliesWhen {
    /// Live, no action needed.
    case immediately
    /// Picked up by `SessionController.beginSession`'s config reload.
    case nextSession
    /// Only read during `applicationDidFinishLaunching`.
    case restart

    var message: String {
        switch self {
        case .immediately: return "Applies immediately."
        case .nextSession: return "Takes effect on your next dictation."
        case .restart: return "Takes effect after you quit and reopen whiz."
        }
    }

    /// Only the restart case is coloured — if everything were red, nothing
    /// would stand out, and the restart case is the one that otherwise looks
    /// like a bug.
    var color: Color {
        self == .restart ? .red : .secondary
    }
}

/// A caption line stating when a setting takes effect.
struct AppliesNote: View {
    var when: AppliesWhen
    var detail: String?

    init(_ when: AppliesWhen, detail: String? = nil) {
        self.when = when
        self.detail = detail
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            if let detail {
                Text(detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Text(when.message)
                .font(.caption)
                .foregroundStyle(when.color)
        }
    }
}

#Preview("Applies-when notes") {
    VStack(alignment: .leading, spacing: 12) {
        AppliesNote(.immediately, detail: "Hotkey is re-registered as you type.")
        AppliesNote(.nextSession, detail: "Language, prompt, VAD and sensitivity.")
        AppliesNote(.restart)
    }
    .padding()
    .frame(width: 360, alignment: .leading)
}
