import SwiftUI

/// The whiz waveform-W monogram, as a SwiftUI `Shape`.
///
/// A direct port of `draw_whiz_logo` from
/// `whiz/dictate/providers/macos_logo.py` — same five unit-square points, same
/// stroke ratio, same round caps and joins, so the mark is pixel-comparable to
/// the shipping PyObjC version. Being a `Shape` means one definition serves the
/// menu bar item, the pill, and any future About panel, instead of the Python
/// arrangement where the menu bar rasterised PNGs and the pill drew directly.
struct WhizLogo: Shape {

    /// Five points tracing the W zigzag: top-left → first valley → middle peak
    /// → second valley → top-right. The middle peak sits lower than the outer
    /// tops so the shape reads as a waveform envelope, not just a bold letter.
    ///
    /// Y values are flipped relative to the AppKit original: NSBezierPath uses a
    /// bottom-left origin, SwiftUI a top-left one.
    private static let points: [CGPoint] = [
        CGPoint(x: 0.10, y: 0.25),
        CGPoint(x: 0.28, y: 0.75),
        CGPoint(x: 0.50, y: 0.45),
        CGPoint(x: 0.72, y: 0.75),
        CGPoint(x: 0.90, y: 0.25),
    ]

    /// Stroke width as a fraction of icon size — thick enough to read at 16px.
    static let strokeRatio: CGFloat = 0.14

    func path(in rect: CGRect) -> Path {
        let side = min(rect.width, rect.height)
        let originX = rect.minX + (rect.width - side) / 2
        let originY = rect.minY + (rect.height - side) / 2

        var path = Path()
        for (index, point) in Self.points.enumerated() {
            let p = CGPoint(x: originX + point.x * side, y: originY + point.y * side)
            index == 0 ? path.move(to: p) : path.addLine(to: p)
        }
        return path
    }
}

/// The logo stroked in a state tint, sized to a square edge.
struct WhizLogoView: View {
    var state: DictationState
    var size: CGFloat

    var body: some View {
        let tint = state.tint
        WhizLogo()
            .stroke(
                Color(.sRGB, red: tint.r, green: tint.g, blue: tint.b, opacity: tint.a),
                style: StrokeStyle(
                    lineWidth: size * WhizLogo.strokeRatio,
                    lineCap: .round,
                    lineJoin: .round
                )
            )
            .frame(width: size, height: size)
    }
}

#Preview("Logo at menu bar and pill sizes") {
    HStack(spacing: 24) {
        WhizLogoView(state: .idle, size: 18)
        WhizLogoView(state: .listening, size: 20)
        WhizLogoView(state: .transcribing, size: 44)
    }
    .padding()
}
