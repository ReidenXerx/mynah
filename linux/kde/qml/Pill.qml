// The floating pill — the macOS app's IndicatorPanel, number for number
// (macos/Sources/MynahApp/UI/IndicatorPanel.swift): a 168×44 capsule, the
// bird (20×20) and five 4-wide bars 6 apart in the state's tint, 14 left
// padding, 12 between; heights 4…22 from the level with a slight fan
// across the bars, eased over 0.08 s; the whole pill fades over 0.18 s.
import QtQuick

Item {
    id: root
    width: 168
    height: 44

    // The state tints (DictationState.swift), the same in light and dark.
    readonly property var tints: ({
        "idle": Qt.rgba(0.60, 0.60, 0.65, 1),
        "loading": Qt.rgba(0.45, 0.55, 0.75, 1),
        "listening": Qt.rgba(0.20, 0.80, 0.95, 1),
        "transcribing": Qt.rgba(0.95, 0.70, 0.20, 1)
    })
    readonly property color tint: tints[Mynah.state] || tints["idle"]

    opacity: pill.shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 180 } }

    // The HUD material: dark and translucent over the compositor's blur,
    // with the 0.5-px white hairline at 8%.
    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Qt.rgba(0.11, 0.11, 0.12, 0.72)
        border.width: 0.5
        border.color: Qt.rgba(1, 1, 1, 0.08)
    }

    Row {
        x: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12

        BirdMark {
            width: 20
            height: 20
            color: root.tint
            anchors.verticalCenter: parent.verticalCenter
        }

        Row {
            spacing: 6
            anchors.verticalCenter: parent.verticalCenter
            Repeater {
                model: 5
                Rectangle {
                    required property int index
                    // IndicatorPanel.swift:126-130
                    readonly property real phase: (index - 2) * 0.35
                    readonly property real amp:
                        Math.max(0, Math.min(1, Mynah.level + phase * 0.15))
                    width: 4
                    radius: 2
                    height: 4 + amp * (22 - 4)
                    color: root.tint
                    anchors.verticalCenter: parent.verticalCenter
                    Behavior on height { NumberAnimation { duration: 80; easing.type: Easing.OutQuad } }
                }
            }
        }
    }
}
