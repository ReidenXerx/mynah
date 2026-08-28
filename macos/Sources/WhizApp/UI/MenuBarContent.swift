import SwiftUI

/// The menu bar dropdown.
///
/// Replaces `macos_rumps.py`, and with it the `rumps` dependency. rumps was
/// pulled in because driving `NSStatusItem` and the AppKit run loop by hand
/// through PyObjC proved unreliable — `macos_menubar.py` was the abandoned
/// first attempt. `MenuBarExtra` is the platform's own answer to the same
/// problem, so both files and the dependency go away together.
struct MenuBarContent: View {
    @ObservedObject var controller: SessionController

    @State private var isAccessibilityTrusted = Permissions.isAccessibilityTrusted
    @State private var launchesAtLogin = LoginItem.isEnabled

    var body: some View {
        Button(controller.isSessionActive ? "Stop Dictation" : "Start Dictation") {
            controller.toggleSession()
        }
        .keyboardShortcut("d")

        Text(controller.state.menuLabel)

        Divider()

        if !isAccessibilityTrusted {
            // Injection silently no-ops without this, so surface it rather than
            // letting dictation appear to work while typing nothing.
            Button("Grant Accessibility…") {
                Permissions.requestAccessibility()
                Permissions.openAccessibilitySettings()
            }
        }

        Toggle("Start at Login", isOn: $launchesAtLogin)
            .onChange(of: launchesAtLogin) { enabled in
                do {
                    enabled ? try LoginItem.enable() : try LoginItem.disable()
                } catch {
                    NSLog("whiz: could not update login item: \(error.localizedDescription)")
                    launchesAtLogin = LoginItem.isEnabled
                }
            }

        Button("Open Config File") {
            NSWorkspace.shared.open(WhizConfig.path)
        }

        Divider()

        Text("whiz \(WhizApp.version) · \(controller.config.hotkey)")

        Button("Quit whiz") {
            NSApplication.shared.terminate(nil)
        }
        .keyboardShortcut("q")
    }
}
