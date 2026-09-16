import SwiftUI

/// Mynah's mark, as a SwiftUI `Shape`: the bird head in profile, beak open
/// mid-word, with the eye cut out of the silhouette.
///
/// One definition serves the menu bar item, the pill and anything else, and it
/// is the same geometry as `docs/assets/mynah-glyph.svg` and the AppKit drawing
/// in `mynah/providers/macos_logo.py` — traced from the same unit-square path,
/// so all three are the same bird.
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

        // crest
            path.move(to: at(0.3594, 0.1602))
            path.addCurve(to: at(0.3789, 0.043), control1: at(0.3359, 0.1133), control2: at(0.3438, 0.0703))
            path.addCurve(to: at(0.4375, 0.1367), control1: at(0.3828, 0.0859), control2: at(0.4023, 0.1172))
            path.closeSubpath()

        // head and shoulder
            path.move(to: at(0.4062, 0.1562))
            path.addCurve(to: at(0.7109, 0.4453), control1: at(0.5781, 0.1562), control2: at(0.7109, 0.2812))
            path.addCurve(to: at(0.6953, 0.5352), control1: at(0.7109, 0.4766), control2: at(0.707, 0.5078))
            path.addLine(to: at(0.7812, 0.5508))
            path.addCurve(to: at(0.793, 0.5977), control1: at(0.8047, 0.5547), control2: at(0.8125, 0.582))
            path.addLine(to: at(0.6758, 0.6953))
            path.addCurve(to: at(0.5, 0.7578), control1: at(0.6289, 0.7344), control2: at(0.5664, 0.7578))
            path.addCurve(to: at(0.1797, 0.4453), control1: at(0.3203, 0.7578), control2: at(0.1797, 0.6211))
            path.addCurve(to: at(0.4062, 0.1562), control1: at(0.1797, 0.2695), control2: at(0.2266, 0.1562))
            path.closeSubpath()

        // beak, open mid-word
            path.move(to: at(0.6797, 0.375))
            path.addLine(to: at(0.9219, 0.3984))
            path.addCurve(to: at(0.9336, 0.4297), control1: at(0.9375, 0.3984), control2: at(0.9453, 0.418))
            path.addLine(to: at(0.8789, 0.4766))
            path.addCurve(to: at(0.8516, 0.4805), control1: at(0.8711, 0.4844), control2: at(0.8594, 0.4844))
            path.addLine(to: at(0.6797, 0.4219))
            path.closeSubpath()
            path.move(to: at(0.6875, 0.4766))
            path.addLine(to: at(0.8906, 0.5078))
            path.addCurve(to: at(0.8906, 0.5352), control1: at(0.9062, 0.5117), control2: at(0.9062, 0.5312))
            path.addLine(to: at(0.7734, 0.5664))
            path.addCurve(to: at(0.7539, 0.5586), control1: at(0.7656, 0.5703), control2: at(0.7578, 0.5664))
            path.addLine(to: at(0.6875, 0.5))
            path.closeSubpath()

        // the eye, as a hole
        let eyeRadius = side * 0.043
        let eyeCentre = at(0.551, 0.344)
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
