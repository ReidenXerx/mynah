// The floating indicator (2026-10-19 concept): the bird in a dark circle
// whose rim pulses with the voice — three wavy rings around it, one each
// for the low, middle and high bands, swelling out and turning slowly, in
// the state's tint. The core draws the bird and the rings (BirdItem with
// `waves`, mynah_bird_render_pill); this draws the circle behind the bird,
// the one place with a backdrop: the rings float over the desktop, a faint
// dark edge keeping them readable. The window is the square around the
// circle (`pill.side`, `pill.circle`; pill.cpp). It fades over 0.18 s.
import QtQuick
import org.mynah

Item {
    id: root
    width: pill.side
    height: pill.side

    opacity: pill.shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 180 } }

    // The HUD material, as the capsule was: dark and translucent over the
    // compositor's blur (pill.cpp blurs this circle only), with the 0.5-px
    // white hairline at 8%.
    Rectangle {
        anchors.centerIn: parent
        width: pill.circle
        height: pill.circle
        radius: width / 2
        color: Qt.rgba(0.11, 0.11, 0.12, 0.72)
        border.width: 0.5
        border.color: Qt.rgba(1, 1, 1, 0.08)
    }

    // Full colour, the state in its pose — the wing up while listening,
    // asleep while the model is unloaded — and in the rings' tint.
    Bird {
        anchors.fill: parent
        waves: true
        engineState: Mynah.state
        modelStatus: Mynah.modelStatus
        level: Mynah.level
        bands: Mynah.bands
    }
}
