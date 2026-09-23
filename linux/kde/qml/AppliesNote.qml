// "When does this apply" under a setting — the macOS app's AppliesNote:
// an optional detail line, then one of three timings; the one that needs a
// restart is red.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: note
    property string detail: ""
    property string when: "next" // "now", "next" or "restart"
    spacing: 2
    Layout.fillWidth: true
    Layout.maximumWidth: 300

    Label {
        visible: note.detail.length > 0
        text: note.detail
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        font: Kirigami.Theme.smallFont
        color: Kirigami.Theme.disabledTextColor
    }
    Label {
        text: note.when === "now" ? "Applies immediately."
            : note.when === "restart" ? "Takes effect after you quit and reopen mynah."
            : "Takes effect on your next dictation."
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        font: Kirigami.Theme.smallFont
        color: note.when === "restart" ? Kirigami.Theme.negativeTextColor
                                       : Kirigami.Theme.disabledTextColor
    }
}
