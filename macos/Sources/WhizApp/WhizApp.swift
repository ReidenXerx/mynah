import SwiftUI

@main
struct WhizApp: App {

    /// Keep in step with `pyproject.toml` and `whiz/__init__.py`.
    static let version = "0.14.0"

    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        MenuBarExtra {
            if let controller = delegate.controller {
                MenuBarContent(controller: controller)
            }
        } label: {
            // MenuBarExtra's label is rendered as a template image, so the tint
            // set here is ignored by the system in favour of the menu bar's own
            // appearance. State is conveyed by the pill; the menu bar item just
            // marks that whiz is running.
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

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {

    private(set) var controller: SessionController?
    private var indicator: IndicatorPanel?
    private let hotkeys = HotkeyManager()

    func applicationDidFinishLaunching(_ notification: Notification) {
        let controller = SessionController()
        self.controller = controller

        if controller.config.showIndicator {
            let indicator = IndicatorPanel(controller: controller)
            indicator.setup()
            self.indicator = indicator
            if controller.config.idleVisible { indicator.show() }
        }

        hotkeys.register(controller.config.hotkey) { [weak self] in
            self?.handleTrigger()
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        controller?.endSession()
        hotkeys.unregister()
    }

    private func handleTrigger() {
        guard let controller else { return }
        controller.toggleSession()

        guard let indicator, controller.config.showIndicator else { return }
        if controller.isSessionActive {
            indicator.show()
        } else if !controller.config.idleVisible {
            indicator.hide()
        }
    }
}
