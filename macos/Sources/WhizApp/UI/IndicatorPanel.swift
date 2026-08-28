import AppKit
import SwiftUI

/// The floating "listening" pill.
///
/// Replaces `macos_indicator.py`. That file was 519 lines, the bulk of it
/// ObjC-runtime glue: a hand-built NSView subclass, `performSelectorOnMainThread:`
/// string dispatch, and a CoreAnimation fallback path added because implicit
/// animations silently failed under launchd. None of that survives the port —
/// SwiftUI redraws from observable state, and a real app bundle has a working
/// animation backend.
///
/// The window-level behaviour does carry over, and each of these flags was
/// learned the hard way:
///   - `.floating` level so it sits above normal windows
///   - `ignoresMouseEvents` so it never steals focus mid-dictation
///   - `hidesOnDeactivate = false` — the shipping bug where an accessory app's
///     panel hid itself and the indicator was simply never visible
///   - `canJoinAllSpaces` so it follows the user across Spaces and full-screen
@MainActor
final class IndicatorPanel {

    private var panel: NSPanel?
    private let controller: SessionController

    private static let width: CGFloat = 168
    private static let height: CGFloat = 44
    private static let bottomInset: CGFloat = 80

    init(controller: SessionController) {
        self.controller = controller
    }

    func setup() {
        guard panel == nil, let screen = NSScreen.main else { return }

        let frame = NSRect(
            x: (screen.frame.width - Self.width) / 2,
            y: Self.bottomInset,
            width: Self.width,
            height: Self.height
        )

        let panel = NSPanel(
            contentRect: frame,
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        panel.level = .floating
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.ignoresMouseEvents = true
        panel.hidesOnDeactivate = false
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        panel.alphaValue = 0

        let host = NSHostingView(rootView: IndicatorView(controller: controller))
        host.frame = NSRect(origin: .zero, size: frame.size)
        panel.contentView = host

        self.panel = panel
    }

    func show() {
        guard let panel else { return }
        panel.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.18
            panel.animator().alphaValue = 1
        }
    }

    func hide() {
        guard let panel else { return }
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.18
            panel.animator().alphaValue = 0
        } completionHandler: {
            // The completion handler runs on the main thread, but is typed as
            // nonisolated, so assert the isolation the runtime already gives us.
            MainActor.assumeIsolated { panel.orderOut(nil) }
        }
    }
}

/// The pill's contents: vibrancy background, W logo, five waveform bars.
struct IndicatorView: View {
    @ObservedObject var controller: SessionController

    private static let barCount = 5
    private static let barWidth: CGFloat = 4
    private static let barSpacing: CGFloat = 6
    private static let barMinHeight: CGFloat = 4
    private static let barMaxHeight: CGFloat = 22

    var body: some View {
        HStack(spacing: 12) {
            WhizLogoView(state: controller.state, size: 20)
            HStack(spacing: Self.barSpacing) {
                ForEach(0..<Self.barCount, id: \.self) { index in
                    Capsule()
                        .fill(tintColor)
                        .frame(width: Self.barWidth, height: barHeight(index))
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(VisualEffectBackground())
        .clipShape(Capsule())
        .overlay(Capsule().strokeBorder(.white.opacity(0.08), lineWidth: 0.5))
        .animation(.easeOut(duration: 0.08), value: controller.level)
    }

    private var tintColor: Color {
        let t = controller.state.tint
        return Color(.sRGB, red: t.r, green: t.g, blue: t.b, opacity: t.a)
    }

    /// Per-bar phase offset so adjacent bars differ and the row reads as a
    /// waveform rather than a single value drawn five times. Same formula as
    /// the AppKit original.
    private func barHeight(_ index: Int) -> CGFloat {
        let phase = (Double(index) - Double(Self.barCount - 1) / 2) * 0.35
        let amplitude = max(0, min(1, controller.level + phase * 0.15))
        return Self.barMinHeight + amplitude * (Self.barMaxHeight - Self.barMinHeight)
    }
}

/// HUD-window vibrancy, so the pill blurs what is behind it.
private struct VisualEffectBackground: NSViewRepresentable {
    func makeNSView(context: Context) -> NSVisualEffectView {
        let view = NSVisualEffectView()
        view.material = .hudWindow
        view.blendingMode = .behindWindow
        view.state = .active
        return view
    }

    func updateNSView(_ nsView: NSVisualEffectView, context: Context) {}
}
