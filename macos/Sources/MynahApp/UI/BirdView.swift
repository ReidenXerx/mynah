import AppKit
import CMynah
import QuartzCore
import SwiftUI

/// The pill's animated bird.
///
/// The core renders it and decides how it moves (`mynah_bird_*` — the same
/// frames and behaviour as the KDE pill, `linux/kde/src/bird_item.cpp`): this
/// only hands it the engine's state and a clock, and shows the bitmap. Full
/// colour, not tinted — the state shows in its pose, the wing up while
/// listening, and in the bars' tint.
struct BirdView: NSViewRepresentable {
    var state: DictationState

    func makeNSView(context: Context) -> BirdNSView {
        let view = BirdNSView(frame: .zero)
        view.state = state
        return view
    }

    func updateNSView(_ nsView: BirdNSView, context: Context) {
        nsView.state = state
    }
}

/// Renders on the main thread — the bird's one thread — into a CGImage that
/// becomes the layer's contents. A ~60 Hz timer runs only while the bird
/// moves; at rest nothing ticks.
final class BirdNSView: NSView {

    private let bird = BirdBox()
    private var timer: Timer?
    private var context: CGContext?
    private var image: CGImage?

    var state: DictationState = .idle {
        didSet {
            guard state != oldValue, let pointer = bird.value else { return }
            mynah_bird_set_state(pointer, state.engineState, CACurrentMediaTime())
            render()
        }
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layerContentsRedrawPolicy = .onSetNeedsDisplay
        // System Settings › Accessibility › Display › Reduce motion: the bird
        // jumps to its end pose. Read live, like the rest of the system.
        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(accessibilityDisplayOptionsChanged),
            name: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification,
            object: nil
        )
        applyReducedMotion()
    }

    required init?(coder: NSCoder) {
        fatalError("BirdNSView is built in code")
    }

    // The bitmap is the layer's contents, set in updateLayer, not drawn.
    override var wantsUpdateLayer: Bool { true }

    override func updateLayer() {
        layer?.contents = image
        layer?.contentsGravity = .resizeAspect
        layer?.contentsScale = window?.backingScaleFactor ?? 1
    }

    override func layout() {
        super.layout()
        render()
    }

    // Onto a window, or a screen with another scale: render for it.
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        render()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        render()
    }

    @objc private func accessibilityDisplayOptionsChanged(_ notification: Notification) {
        applyReducedMotion()
    }

    @objc private func tick() {
        render()
    }

    private func applyReducedMotion() {
        guard let pointer = bird.value else { return }
        let reduced = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        mynah_bird_set_reduced_motion(pointer, reduced ? 1 : 0)
        render()
    }

    /// Render the bird as it is now; keep the timer running while it moves.
    private func render() {
        guard let pointer = bird.value, let window else {
            stopTimer()
            return
        }
        // Device pixels, so the bird is sharp on a Retina screen.
        let scale = window.backingScaleFactor
        let width = Int((bounds.width * scale).rounded(.up))
        let height = Int((bounds.height * scale).rounded(.up))
        guard width > 0, height > 0 else {
            stopTimer()
            return
        }
        if context?.width != width || context?.height != height {
            // Premultiplied ARGB in 32-bit little-endian words — exactly the
            // 0xAARRGGBB uint32s mynah_bird_render writes, rows width * 4
            // bytes apart.
            context = CGContext(
                data: nil, width: width, height: height,
                bitsPerComponent: 8, bytesPerRow: width * 4,
                space: CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                    | CGBitmapInfo.byteOrder32Little.rawValue
            )
        }
        guard let context, context.bytesPerRow == width * 4, let pixels = context.data else {
            stopTimer()
            return
        }
        let moving = mynah_bird_render(
            pointer, CACurrentMediaTime(),
            pixels.assumingMemoryBound(to: UInt32.self), Int32(width), Int32(height)
        )
        image = moving < 0 ? nil : context.makeImage()
        needsDisplay = true
        if moving == 1 { startTimer() } else { stopTimer() }
    }

    private func startTimer() {
        guard timer == nil else { return }
        // On the main run loop in .common modes, so it keeps going while a
        // menu is open; selector-based, so nothing crosses an isolation
        // boundary.
        let timer = Timer(timeInterval: 1.0 / 60, target: self, selector: #selector(tick),
                          userInfo: nil, repeats: true)
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    private func stopTimer() {
        timer?.invalidate()
        timer = nil
    }
}

/// The core's bird, freed with its owner. A Sendable box like `EngineBox`, so
/// the free happens in its own nonisolated deinit without touching main-actor
/// state; the pointer is only ever used on the main thread.
final class BirdBox: @unchecked Sendable {
    let value: OpaquePointer? = mynah_bird_create()

    deinit {
        if let value { mynah_bird_destroy(value) }
    }
}

extension DictationState {
    /// The engine's own value, for the core's bird.
    var engineState: mynah_state {
        switch self {
        case .idle:         return MYNAH_IDLE
        case .loading:      return MYNAH_LOADING
        case .listening:    return MYNAH_LISTENING
        case .transcribing: return MYNAH_TRANSCRIBING
        }
    }
}
