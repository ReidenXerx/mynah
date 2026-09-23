// The mynah bird mark (docs/assets/mynah-glyph.svg), one even-odd path —
// the eye is a hole — scaled from its 208-unit viewBox to this item's size.
import QtQuick
import QtQuick.Shapes

Item {
    id: mark
    property color color: "#14120F"

    Shape {
        width: 208
        height: 208
        preferredRendererType: Shape.CurveRenderer
        transform: Scale { xScale: mark.width / 208; yScale: mark.height / 208 }

        ShapePath {
            fillColor: mark.color
            strokeWidth: -1
            fillRule: ShapePath.OddEvenFill
            // The SVG's viewBox starts at (38, -10): moved to the origin.
            PathSvg {
                path: "M140 96 C138 68 118 46 88 44 C48 42 14 74 14 114 C14 146 32 172 60 182 C86 192 114 186 130 168 C138 150 141 122 140 96 Z M66 50 C58 32 60 20 72 12 C74 26 80 36 90 42 Z M94 40 C90 22 96 12 110 10 C108 24 110 34 116 44 Z M50 60 C36 48 32 36 40 26 C46 38 54 46 64 50 Z M138 88 L186 102 C194 105 194 115 186 118 L138 130 C134 116 134 102 138 88 Z M88 102 a24 24 0 1 0 48 0 a24 24 0 1 0 -48 0 z"
            }
        }
    }
}
