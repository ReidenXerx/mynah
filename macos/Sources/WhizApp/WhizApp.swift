import AppKit
import Combine
import SwiftUI

@main
struct WhizApp: App {

    /// Keep in step with `pyproject.toml` and `whiz/__init__.py`.
    static let version = "0.14.0"

    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        MenuBarExtra {
            // `delegate.controller` is a non-optional `let` created with the
            // delegate. It used to be an optional assigned in
            // `applicationDidFinishLaunching`, which meant SwiftUI rendered this
            // content once while it was still nil and never re-evaluated —
            // producing an empty menu that would not open at all.
            MenuBarContent(
                controller: delegate.controller,
                onOpenSettings: { delegate.showSettings() })
        } label: {
            // MenuBarExtra's label is rendered as a template image, so the tint
            // is ignored in favour of the menu bar's own appearance. State is
            // conveyed by the pill; the menu bar item just marks that whiz is
            // running.
            Image(nsImage: WhizApp.menuBarIcon)
        }
        .menuBarExtraStyle(.menu)
    }

    private static var menuBarIcon: NSImage {
        let size = NSSize(width: 18, height: 18)
        let image = NSImage(size: size, flipped: false) { rect in
            let path = NSBezierPath()
            let points: [(CGFloat, CGFloat)] = [
                (0.10, 0.75), (0.28, 0.25), (0.50, 0.55), (0.72, 0.25), (0.90, 0.75),
            ]
            for (index, point) in points.enumerated() {
                let p = NSPoint(x: rect.width * point.0, y: rect.height * point.1)
                index == 0 ? path.move(to: p) : path.line(to: p)
            }
            path.lineWidth = rect.width * WhizLogo.strokeRatio
            path.lineCapStyle = .round
            path.lineJoinStyle = .round
            NSColor.black.set()
            path.stroke()
            return true
        }
        image.isTemplate = true
        return image
    }
}

/// Owns the app's long-lived objects and bridges the hotkey to the session.
///
/// `ObservableObject` matters: `@NSApplicationDelegateAdaptor` observes the
/// delegate when it conforms, which is what lets the menu re-render as state
/// changes.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, ObservableObject {

    /// Created eagerly rather than in `applicationDidFinishLaunching`, so the
    /// menu has real content from the very first render.
    let controller = SessionController()

    private var indicator: IndicatorPanel?
    private lazy var settings = SettingsWindow(controller: controller)
    private let hotkeys = HotkeyManager()
    private var cancellables = Set<AnyCancellable>()
    private var permissionTimer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        Log.ui.notice("launching whiz \(WhizApp.version, privacy: .public)")

        if controller.config.showIndicator {
            let indicator = IndicatorPanel(controller: controller)
            indicator.setup()
            self.indicator = indicator
            if controller.config.idleVisible { indicator.show() }
        }

        // Drive the pill from observed state rather than imperatively after the
        // hotkey. Session start is async (a cold model load takes seconds), so
        // reading `isSessionActive` immediately after `toggleSession()` saw the
        // old value and hid the pill the instant it was asked to appear.
        controller.$isSessionActive
            .removeDuplicates()
            .sink { [weak self] active in self?.updateIndicator(visible: active) }
            .store(in: &cancellables)

        // TCC offers no change notification, so poll. Cheap, and it means the
        // menu reflects a grant made in System Settings without a relaunch.
        permissionTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { _ in
            Task { @MainActor in self.controller.refreshPermissions() }
        }

        registerHotkey(controller.config.hotkey)

        // Re-register when the hotkey is edited in Settings, so it takes effect
        // without a restart.
        controller.$config
            .map(\.hotkey)
            .removeDuplicates()
            .dropFirst()
            .sink { [weak self] hotkey in self?.registerHotkey(hotkey) }
            .store(in: &cancellables)
    }

    func applicationWillTerminate(_ notification: Notification) {
        controller.endSession()
        hotkeys.unregister()
        permissionTimer?.invalidate()
        // Must come last: ggml aborts at exit if a model is still loaded.
        controller.shutdownBlocking()
    }

    func showSettings() {
        settings.show()
    }

    private func registerHotkey(_ hotkey: String) {
        if hotkeys.register(hotkey, onTrigger: { [weak self] in self?.handleTrigger() }) {
            Log.ui.notice("hotkey registered: \(hotkey, privacy: .public)")
        } else {
            Log.ui.error("hotkey registration FAILED: \(hotkey, privacy: .public)")
            controller.reportError(
                "Could not register the hotkey '\(hotkey)'. Another app may already use it.")
        }
    }

    private func handleTrigger() {
        Log.ui.notice("hotkey fired")
        controller.toggleSession()
    }

    private func updateIndicator(visible: Bool) {
        guard let indicator, controller.config.showIndicator else { return }
        if visible {
            indicator.show()
        } else if !controller.config.idleVisible {
            indicator.hide()
        }
    }
}
