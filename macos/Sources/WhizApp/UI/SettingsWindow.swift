import AppKit
import SwiftUI

/// Hosts `SettingsView` in a real window.
///
/// Built by hand rather than using SwiftUI's `Settings` scene. whiz is an
/// accessory app (`LSUIElement`), so it has no menu bar of its own and no
/// standard "Settings…" item for that scene to hook into; opening it would mean
/// poking `showSettingsWindow:` through `NSApp.sendAction`, whose selector name
/// has already changed once across macOS releases. An `NSWindowController` is a
/// few more lines and behaves predictably.
///
/// The activation dance matters too: an accessory app is never "active", so a
/// window it orders front sits behind whatever the user was using. Temporarily
/// switching to `.regular` gives it a Dock icon and lets it come forward
/// properly, and it reverts on close.
@MainActor
final class SettingsWindow: NSObject, NSWindowDelegate {

    private var window: NSWindow?
    private let controller: SessionController

    init(controller: SessionController) {
        self.controller = controller
    }

    func show() {
        if window == nil { build() }

        // Become a regular app so the window can take focus, then restore
        // accessory status when it closes (see windowWillClose).
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
        window?.center()
    }

    private func build() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 460, height: 340),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = "whiz Settings"
        window.contentView = NSHostingView(rootView: SettingsView(controller: controller))
        window.isReleasedWhenClosed = false  // reuse the instance across opens
        window.delegate = self
        self.window = window
    }

    func windowWillClose(_ notification: Notification) {
        // Back to a menu bar accessory: no Dock icon, no app switcher entry.
        NSApp.setActivationPolicy(.accessory)
    }
}
