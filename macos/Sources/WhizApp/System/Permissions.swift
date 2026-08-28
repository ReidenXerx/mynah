import AppKit
import ApplicationServices

/// Accessibility permission — required for CGEvent posting (text injection).
///
/// The Python version had to poll for up to five minutes after prompting,
/// because the LaunchAgent had no UI to report status and launchd would
/// otherwise crash-loop it. A real app can just observe and reflect the state
/// in its own window, so this exposes a plain check plus a prompt.
///
/// The bundle is what makes this durable: TCC keys the grant to the app's
/// signed identity, so it survives updates. That replaces the entire
/// `_ensure_runner()` dance in `service.py`, which existed only to keep a
/// copied Python binary at a stable path so the grant would not be revoked.
enum Permissions {

    static var isAccessibilityTrusted: Bool {
        AXIsProcessTrusted()
    }

    /// Prompt for Accessibility, opening System Settings to the right pane.
    @discardableResult
    static func requestAccessibility() -> Bool {
        // `kAXTrustedCheckOptionPrompt` is imported as a mutable global, so
        // Swift 6 rejects touching it directly. Its value is a documented,
        // stable constant, so use the literal.
        let options = ["AXTrustedCheckOptionPrompt": true]
        return AXIsProcessTrustedWithOptions(options as CFDictionary)
    }

    static func openAccessibilitySettings() {
        let url = URL(
            string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!
        NSWorkspace.shared.open(url)
    }

    static func openMicrophoneSettings() {
        let url = URL(
            string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone")!
        NSWorkspace.shared.open(url)
    }
}
