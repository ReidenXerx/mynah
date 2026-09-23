// mynah Settings — the macOS app's SettingsView (General, Recognition,
// Sensitivity), control for control where Linux has the same thing; every
// edit goes to the engine key by key (Mynah.setConfig) and the controls
// show what it accepted. Linux differences: the shortcut is KDE's (KGlobal-
// Accel, with a key recorder), push-to-talk is offered, and the model
// section lists the Linux models and where speech runs.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kquickcontrols as KQuickControls

ApplicationWindow {
    id: window
    title: "mynah Settings"
    width: 460
    height: 340
    minimumWidth: 460
    minimumHeight: 340
    visible: false

    readonly property var cfg: Mynah.config

    // A slider bound to one numeric setting, saved when released, with the
    // value on the right in a fixed-width label (monospaced digits).
    component ConfigSlider: RowLayout {
        id: row
        property string key
        property real from: 0
        property real to: 1
        property real step: 0
        property int decimals: 3
        property string suffix: ""
        property string zeroText: ""
        function refresh() { slider.value = Number(Mynah.config[row.key]) }
        Slider {
            id: slider
            from: row.from
            to: row.to
            stepSize: row.step
            Layout.fillWidth: true
            Component.onCompleted: row.refresh()
            onPressedChanged: if (!pressed) Mynah.setConfig(row.key, value)
        }
        Label {
            Layout.preferredWidth: 56
            horizontalAlignment: Text.AlignRight
            font.family: "monospace"
            text: row.zeroText.length && slider.value === 0 ? row.zeroText
                                                            : slider.value.toFixed(row.decimals) + row.suffix
        }
        Connections {
            target: Mynah
            function onConfigChanged() { row.refresh() }
        }
    }

    header: TabBar {
        id: tabs
        TabButton { text: "General"; icon.name: "configure" }
        TabButton { text: "Recognition"; icon.name: "audio-input-microphone" }
        TabButton { text: "Sensitivity"; icon.name: "audio-volume-high" }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: tabs.currentIndex

        // --- General -------------------------------------------------------------------
        ScrollView {
            contentWidth: availableWidth
            Kirigami.FormLayout {
                width: parent.width

                KQuickControls.KeySequenceItem {
                    Kirigami.FormData.label: "Shortcut:"
                    keySequence: shortcut.keySequence
                    showClearButton: false
                    onKeySequenceModified: shortcut.setKeySequence(keySequence)
                }
                AppliesNote {
                    when: "now"
                    detail: "Also in System Settings → Keyboard → Shortcuts, under mynah."
                }
                ComboBox {
                    Kirigami.FormData.label: "The shortcut:"
                    model: ["Starts and stops dictation", "Dictates while held"]
                    currentIndex: window.cfg.trigger === "ptt" ? 1 : 0
                    onActivated: (index) => Mynah.setConfig("trigger", index === 1 ? "ptt" : "toggle")
                }
                AppliesNote { when: "now" }

                Item { Kirigami.FormData.isSection: true }

                CheckBox {
                    text: "Show floating indicator"
                    checked: window.cfg.show_indicator === true
                    onToggled: Mynah.setConfig("show_indicator", checked)
                }
                CheckBox {
                    text: "Keep indicator visible when idle"
                    enabled: window.cfg.show_indicator === true
                    checked: window.cfg.idle_visible === true
                    onToggled: Mynah.setConfig("idle_visible", checked)
                }
                AppliesNote { when: "restart" }

                Item { Kirigami.FormData.isSection: true }

                ConfigSlider {
                    Kirigami.FormData.label: "Unload model after:"
                    key: "idle_timeout"
                    from: 0; to: 300; step: 15; decimals: 0; suffix: "s"; zeroText: "never"
                    Layout.preferredWidth: 280
                }
                AppliesNote {
                    detail: "Keeps the model warm for back-to-back dictation, then frees the memory."
                }
            }
        }

        // --- Recognition ---------------------------------------------------------------
        ScrollView {
            contentWidth: availableWidth
            Kirigami.FormLayout {
                width: parent.width

                ComboBox {
                    id: language
                    Kirigami.FormData.label: "Language:"
                    model: languages
                    textRole: "name"
                    valueRole: "code"
                    Component.onCompleted: currentIndex = indexOfValue(window.cfg.language)
                    onActivated: Mynah.setConfig("language", currentValue)
                    Connections {
                        target: Mynah
                        function onConfigChanged() {
                            language.currentIndex = language.indexOfValue(window.cfg.language)
                        }
                    }
                }
                AppliesNote {}

                ComboBox {
                    Kirigami.FormData.label: "Transcribe:"
                    model: ["While speaking", "On session end"]
                    currentIndex: window.cfg.transcription_mode === "on_stop" ? 1 : 0
                    onActivated: (index) => Mynah.setConfig("transcription_mode",
                                                            index === 1 ? "on_stop" : "live")
                }
                AppliesNote {
                    detail: "While speaking: text lands each time you pause. On session end: the whole recording is transcribed in one pass when you stop — text arrives later, but each passage is decoded with full context."
                }

                Item { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Speech model" }

                Repeater {
                    model: models.models
                    ColumnLayout {
                        required property var modelData
                        readonly property bool busy: models.downloading === modelData.alias
                        Layout.fillWidth: true
                        Layout.maximumWidth: 300
                        spacing: 2
                        RowLayout {
                            Label {
                                text: modelData.title
                                font.bold: modelData.inUse
                                Layout.fillWidth: true
                            }
                            Label {
                                visible: modelData.onDisk
                                text: modelData.inUse ? "In use" : "Installed"
                                color: Kirigami.Theme.disabledTextColor
                            }
                            Button {
                                visible: !modelData.onDisk && !parent.parent.busy
                                enabled: models.downloading === ""
                                text: "Download"
                                onClicked: models.download(modelData.alias)
                            }
                        }
                        Label {
                            text: modelData.detail + " About " + models.formatBytes(modelData.bytes) + "."
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.disabledTextColor
                        }
                        RowLayout {
                            visible: parent.busy
                            ProgressBar {
                                from: 0
                                to: models.total
                                value: models.received
                                Layout.fillWidth: true
                            }
                            Button { text: "Cancel"; onClicked: models.cancel() }
                        }
                        Label {
                            visible: parent.busy
                            text: models.received > 0
                                  ? models.formatBytes(models.received) + " of " + models.formatBytes(models.total)
                                  : "Starting…"
                            font: Kirigami.Theme.smallFont
                        }
                    }
                }
                Label {
                    visible: models.error.length > 0
                    text: models.error
                    color: Kirigami.Theme.negativeTextColor
                    wrapMode: Text.WordWrap
                    Layout.maximumWidth: 300
                }
                Label {
                    Kirigami.FormData.label: "Runs on:"
                    text: models.device
                }
                CheckBox {
                    text: "Prefer the discrete GPU"
                    checked: window.cfg.gpu === true
                    onToggled: Mynah.setConfig("gpu", checked)
                }
                AppliesNote {
                    detail: "Off: the integrated GPU, or the CPU. The discrete GPU sleeps about ten seconds after each sentence either way. Applies when the model next loads."
                }

                Item { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Voice activity detection" }

                CheckBox {
                    text: "Reject non-speech audio"
                    enabled: models.vadOnDisk
                    checked: window.cfg.vad === true
                    onToggled: Mynah.setConfig("vad", checked)
                }
                RowLayout {
                    visible: !models.vadOnDisk
                    Label { text: "Silero model not installed (0.9 MB)." }
                    Button {
                        text: "Download"
                        enabled: models.downloading === ""
                        onClicked: models.download("vad")
                    }
                }
                AppliesNote {
                    visible: models.vadOnDisk
                    detail: "Uses Silero to check an utterance is a human voice, not just loud. Rejects fans, keyboards and vacuum cleaners before transcription."
                }

                Item { Kirigami.FormData.isSection: true }

                TextArea {
                    id: prompt
                    Kirigami.FormData.label: "Prompt:"
                    Layout.preferredWidth: 280
                    Layout.preferredHeight: 60
                    wrapMode: TextEdit.Wrap
                    font.family: "monospace"
                    text: window.cfg.prompt || ""
                    onEditingFinished: if (text !== (window.cfg.prompt || "")) Mynah.setConfig("prompt", text)
                }
                AppliesNote {
                    detail: "Biases recognition. Leave empty for the built-in Russian prompt that stops Whisper censoring slang and obscenity."
                }
            }
        }

        // --- Sensitivity ---------------------------------------------------------------
        ScrollView {
            contentWidth: availableWidth
            Kirigami.FormLayout {
                width: parent.width

                ConfigSlider {
                    Kirigami.FormData.label: "Frame energy:"
                    key: "frame_energy"; from: 0.001; to: 0.05; decimals: 3
                    Layout.preferredWidth: 280
                }
                ConfigSlider {
                    Kirigami.FormData.label: "Utterance energy:"
                    key: "min_energy"; from: 0.001; to: 0.05; decimals: 3
                    Layout.preferredWidth: 280
                }
                ConfigSlider {
                    Kirigami.FormData.label: "Min utterance:"
                    key: "min_utterance"; from: 0.05; to: 1.0; decimals: 2; suffix: "s"
                    Layout.preferredWidth: 280
                }
                AppliesNote {
                    detail: "Lower values are more sensitive. These are floors — mynah measures the room at the start of each session and raises them if it is noisy.\n\nIf you have to raise your voice, check the input volume first (System Settings → Sound); a quiet mic loses detail that no setting here can recover."
                }
                Button {
                    text: "Restore Defaults"
                    onClicked: {
                        Mynah.setConfig("frame_energy", 0.010)
                        Mynah.setConfig("min_energy", 0.008)
                        Mynah.setConfig("min_utterance", 0.25)
                    }
                }
            }
        }
    }
}
