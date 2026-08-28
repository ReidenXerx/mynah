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

    @State private var launchesAtLogin = LoginItem.isEnabled

    var body: some View {
        Button(controller.isSessionActive ? "Stop Dictation" : "Start Dictation") {
            controller.toggleSession()
        }
        .keyboardShortcut("d")

        Text(controller.state.menuLabel)

        // Failures used to be invisible: a missing model or denied microphone
        // set `lastError` and nothing ever showed it, so the app just silently
        // did nothing.
        if let error = controller.lastError {
            Divider()
            Text(error)
        }

        Divider()

        // Always shown, with state. Hiding it once granted made it impossible
        // to tell "granted" from "the menu is broken" — and while the menu was
        // broken, this was the only route to granting it.
        if controller.isAccessibilityTrusted {
            Text("Accessibility: granted")
        } else {
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

        Button("Reveal Log…") {
            // `log stream` needs a terminal; opening Console filtered to our
            // subsystem is the closest one-click equivalent.
            NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Applications/Utilities/Console.app"))
        }

        Divider()

        Text("whiz \(WhizApp.version) · \(controller.config.hotkey)")

        Button("Quit whiz") {
            NSApplication.shared.terminate(nil)
        }
        .keyboardShortcut("q")
    }
}
