import SwiftUI

/// Mynah's mark, as a SwiftUI `Shape`: a round-headed bird with a stubby bill,
/// too much eye and a crest that will not lie flat, with the eye cut out of the
/// silhouette.
///
/// One definition serves the menu bar item, the pill and anything else. The
/// path is GENERATED from the same source as `docs/assets/mynah-glyph.svg`, the
/// Omarchy plugin's QML and the AppKit drawing in
/// `mynah/providers/macos_logo.py` — hand-transcribing it is how three copies
/// of one bird drift apart. Do not edit the coordinates here.
///
/// The eye is a hole rather than a second colour: filled with `.evenOdd`, the
/// mark stays legible as one silhouette at 16px in a menu bar, in either
/// appearance, and needs no second tint.
struct MynahLogo: Shape {

    /// Filled, not stroked — and with the even-odd rule, so the eye subpath
    /// punches a hole instead of painting over it.
    static let fillStyle = FillStyle(eoFill: true, antialiased: true)

    func path(in rect: CGRect) -> Path {
        let side = min(rect.width, rect.height)
        let originX = rect.minX + (rect.width - side) / 2
        let originY = rect.minY + (rect.height - side) / 2
        func at(_ x: CGFloat, _ y: CGFloat) -> CGPoint {
            CGPoint(x: originX + x * side, y: originY + y * side)
        }

        var path = Path()

        // the head
        path.move(to: at(0.6731, 0.4615))
        path.addCurve(to: at(0.4231, 0.2115), control1: at(0.6635, 0.3269), control2: at(0.5673, 0.2212))
        path.addCurve(to: at(0.0673, 0.5481), control1: at(0.2308, 0.2019), control2: at(0.0673, 0.3558))
        path.addCurve(to: at(0.2885, 0.875), control1: at(0.0673, 0.7019), control2: at(0.1538, 0.8269))
        path.addCurve(to: at(0.625, 0.8077), control1: at(0.4135, 0.9231), control2: at(0.5481, 0.8942))
        path.addCurve(to: at(0.6731, 0.4615), control1: at(0.6635, 0.7212), control2: at(0.6779, 0.5865))
        path.closeSubpath()
        // the crest, three feathers
        path.move(to: at(0.3173, 0.2404))
        path.addCurve(to: at(0.3462, 0.0577), control1: at(0.2788, 0.1538), control2: at(0.2885, 0.0962))
        path.addCurve(to: at(0.4327, 0.2019), control1: at(0.3558, 0.125), control2: at(0.3846, 0.1731))
        path.closeSubpath()
        path.move(to: at(0.4519, 0.1923))
        path.addCurve(to: at(0.5288, 0.0481), control1: at(0.4327, 0.1058), control2: at(0.4615, 0.0577))
        path.addCurve(to: at(0.5577, 0.2115), control1: at(0.5192, 0.1154), control2: at(0.5288, 0.1635))
        path.closeSubpath()
        path.move(to: at(0.2404, 0.2885))
        path.addCurve(to: at(0.1923, 0.125), control1: at(0.1731, 0.2308), control2: at(0.1538, 0.1731))
        path.addCurve(to: at(0.3077, 0.2404), control1: at(0.2212, 0.1827), control2: at(0.2596, 0.2212))
        path.closeSubpath()
        // the bill: short, straight, blunt
        path.move(to: at(0.6635, 0.4231))
        path.addLine(to: at(0.8942, 0.4904))
        path.addCurve(to: at(0.8942, 0.5673), control1: at(0.9327, 0.5048), control2: at(0.9327, 0.5529))
        path.addLine(to: at(0.6635, 0.625))
        path.addCurve(to: at(0.6635, 0.4231), control1: at(0.6442, 0.5577), control2: at(0.6442, 0.4904))
        path.closeSubpath()

        // the eye, as a hole
        let eyeRadius = side * 0.1154
        let eyeCentre = at(0.5385, 0.4904)
        path.addEllipse(in: CGRect(
            x: eyeCentre.x - eyeRadius,
            y: eyeCentre.y - eyeRadius,
            width: eyeRadius * 2,
            height: eyeRadius * 2
        ))

        return path
    }
}

/// The mark filled in a state tint, sized to a square edge.
struct MynahLogoView: View {
    var state: DictationState
    var size: CGFloat

    var body: some View {
        let tint = state.tint
        MynahLogo()
            .fill(
                Color(.sRGB, red: tint.r, green: tint.g, blue: tint.b, opacity: tint.a),
                style: MynahLogo.fillStyle
            )
            .frame(width: size, height: size)
    }
}

#Preview("The mark at menu bar and pill sizes") {
    HStack(spacing: 24) {
        MynahLogoView(state: .idle, size: 18)
        MynahLogoView(state: .listening, size: 20)
        MynahLogoView(state: .transcribing, size: 44)
    }
    .padding()
}
