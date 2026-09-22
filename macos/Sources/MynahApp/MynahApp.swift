import AppKit
import Combine
import CMynah
import SwiftUI

@main
struct MynahApp: App {

    /// P9: one version number, from the CMake project, reported by the core.
    static var version: String { String(cString: mynah_version()) }

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
            // conveyed by the pill; the menu bar item just marks that mynah is
            // running.
            Image(nsImage: MynahApp.menuBarIcon)
        }
        .menuBarExtraStyle(.menu)
    }

    private static var menuBarIcon: NSImage {
        let size = NSSize(width: 18, height: 18)
        let image = NSImage(size: size, flipped: false) { rect in
            guard let context = NSGraphicsContext.current?.cgContext else { return false }
            context.saveGState()
            // MynahLogo is defined top-left down, the way the SVG and the
            // AppKit indicator draw it; an NSImage context is bottom-up.
            context.translateBy(x: 0, y: rect.height)
            context.scaleBy(x: 1, y: -1)
            context.addPath(MynahLogo().path(in: rect).cgPath)
            NSColor.black.setFill()
            // Even-odd, so the eye stays a hole rather than being filled in.
            context.fillPath(using: .evenOdd)
            context.restoreGState()
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
        Log.ui.notice("launching mynah \(MynahApp.version, privacy: .public)")

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

        registerHotkey(controller.config.hotkey, trigger: controller.config.trigger)

        // Re-register when the hotkey or the trigger is edited in Settings,
        // so they take effect without a restart. Push-to-talk needs the
        // key-up event; toggle does not.
        controller.$config
            .map { ConfigTriggerChange(hotkey: $0.hotkey, trigger: $0.trigger) }
            .removeDuplicates()
            .dropFirst()
            .sink { [weak self] change in
                self?.registerHotkey(change.hotkey, trigger: change.trigger)
            }
            .store(in: &cancellables)
    }

    func applicationWillTerminate(_ notification: Notification) {
        controller.endSession()
        hotkeys.unregister()
        permissionTimer?.invalidate()
        // Must come last: destroy joins the engine's workers and frees the
        // whisper + VAD contexts — ggml aborts at exit if a Metal context is
        // still alive.
        controller.shutdownBlocking()
    }

    func showSettings() {
        settings.show()
    }

    private func registerHotkey(_ hotkey: String, trigger: String) {
        // Push-to-talk: hold to dictate, release to stop — the hotkey
        // manager's key-up event. Toggle semantics are the default.
        let isPTT = trigger == "ptt"
        let registered = hotkeys.register(
            hotkey,
            onPress: { [weak self] in
                isPTT ? self?.controller.startSession() : self?.handleToggle()
            },
            onRelease: isPTT
                ? { [weak self] in self?.controller.endSession() }
                : nil
        )
        if registered {
            Log.ui.notice("hotkey registered: \(hotkey, privacy: .public) (\(trigger, privacy: .public))")
        } else {
            Log.ui.error("hotkey registration FAILED: \(hotkey, privacy: .public)")
            controller.reportError(
                "Could not register the hotkey '\(hotkey)'. Another app may already use it.")
        }
    }

    private func handleToggle() {
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

#if DEBUG
/// A hotkey + trigger pair that can travel through `removeDuplicates()`
/// (Swift tuples do not conform).
struct ConfigTriggerChange: Equatable {
    var hotkey: String
    var trigger: String
}
#endif
