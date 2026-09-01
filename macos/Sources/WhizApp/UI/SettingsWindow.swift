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
///
/// Spaces need explicit handling on top of that — see `moveToActiveSpace` in
/// `build()` and the ordering comment in `show()`.
@MainActor
final class SettingsWindow: NSObject, NSWindowDelegate {

    private var window: NSWindow?
    private let controller: SessionController

    init(controller: SessionController) {
        self.controller = controller
    }

    func show() {
        let isNew = window == nil
        if isNew { build() }
        guard let window else { return }

        // Become a regular app so the window can take focus and standard text
        // editing shortcuts work, then restore accessory status on close (see
        // windowWillClose).
        NSApp.setActivationPolicy(.regular)

        // Order the window in *before* activating. Activating first asks macOS
        // to bring the app forward while its only window still belongs to
        // whichever Space it was last shown on — so macOS switched Spaces to go
        // find it, dragging the user to the Desktop while the window stayed
        // behind. Placing the window on the active Space first (see
        // `moveToActiveSpace` in build()) means activation has something local
        // to raise.
        window.makeKeyAndOrderFront(nil)
        // Only centre on first open, so a window the user has moved stays put.
        if isNew { window.center() }
        NSApp.activate(ignoringOtherApps: true)
    }

    private func build() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 460, height: 340),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = "whiz Settings"
        // Follow the user between Spaces rather than pulling them to wherever
        // the window was last shown. Without this, opening Settings from a
        // second Space switched the user to the Desktop and left the window
        // behind on the Space it came from.
        window.collectionBehavior = [.moveToActiveSpace, .fullScreenAuxiliary]
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
