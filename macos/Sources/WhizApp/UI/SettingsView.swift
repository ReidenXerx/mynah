import SwiftUI

/// The settings window contents.
///
/// Only exposes settings the Swift app actually honours. `dictate_trigger`
/// (push-to-talk) and `dictate_auto_stop_silence` exist in the config file and
/// are implemented in the Python engine, but not here yet — showing controls for
/// them would be worse than omitting them, because a switch that silently does
/// nothing is indistinguishable from a bug.
///
/// Edits write to `~/.config/whiz/config.toml` immediately, preserving the keys
/// the Python CLI owns. Most take effect on the next dictation session; the ones
/// that cannot are marked in the UI rather than left for the user to discover.
struct SettingsView: View {
    @ObservedObject var controller: SessionController

    var body: some View {
        TabView {
            general.tabItem { Label("General", systemImage: "gearshape") }
            recognition.tabItem { Label("Recognition", systemImage: "waveform") }
            sensitivity.tabItem { Label("Sensitivity", systemImage: "mic") }
        }
        .frame(width: 460, height: 340)
    }

    // MARK: - General

    private var general: some View {
        Form {
            Section {
                LabeledContent("Hotkey") {
                    CommittedTextField(
                        value: binding(\.hotkey),
                        validate: { spec in
                            HotkeySpec.parse(spec) == nil
                                ? "Not a valid hotkey. Use e.g. <cmd>+<shift>+. or <f8>."
                                : nil
                        },
                        width: 160)
                }
                AppliesNote(.immediately,
                            detail: "pynput syntax, e.g. <cmd>+<shift>+. or <f8>. "
                                  + "Saved on Return, or when you click away.")
            }

            Section {
                Toggle("Show floating indicator", isOn: binding(\.showIndicator))
                Toggle("Keep indicator visible when idle", isOn: binding(\.idleVisible))
                    .disabled(!controller.config.showIndicator)
                // The panel is built once in applicationDidFinishLaunching, so
                // unlike every other setting this one cannot be picked up by the
                // per-session config reload.
                AppliesNote(.restart)
            }

            Section {
                LabeledContent("Unload model after") {
                    HStack {
                        Slider(value: binding(\.idleTimeout), in: 0...300, step: 15)
                        Text(controller.config.idleTimeout == 0
                             ? "never"
                             : "\(Int(controller.config.idleTimeout))s")
                            .monospacedDigit()
                            .frame(width: 52, alignment: .trailing)
                    }
                }
                AppliesNote(.nextSession,
                            detail: "Keeps the model warm for back-to-back dictation, "
                                  + "then frees the memory.")
            }
        }
        .formStyle(.grouped)
    }

    // MARK: - Recognition

    private var recognition: some View {
        Form {
            Section {
                Picker("Language", selection: binding(\.language)) {
                    // An unrecognised value from a hand-edited config still needs
                    // an entry, or the Picker would silently show the first item
                    // and overwrite it on the next save.
                    if !WhisperLanguages.isKnown(controller.config.language) {
                        Text(WhisperLanguages.language(for: controller.config.language).label)
                            .tag(controller.config.language)
                    }
                    ForEach(WhisperLanguages.all) { language in
                        Text(language.label).tag(language.code)
                    }
                }
                AppliesNote(.nextSession)
            }

            ModelSectionView(controller: controller)

            Section {
                Text("Prompt")
                PromptEditor(text: binding(\.prompt))
                AppliesNote(.nextSession,
                            detail: "Biases recognition. Leave empty for the built-in Russian "
                                  + "prompt that stops Whisper censoring slang and obscenity.")
            }
        }
        .formStyle(.grouped)
    }

    // MARK: - Sensitivity

    private var sensitivity: some View {
        Form {
            Section {
                slider("Frame energy", binding(\.frameEnergy), range: 0.001...0.05, digits: 3)
                slider("Utterance energy", binding(\.minEnergy), range: 0.001...0.05, digits: 3)
                slider("Min utterance", binding(\.minUtterance), range: 0.05...1.0, digits: 2, unit: "s")
            } footer: {
                AppliesNote(.nextSession,
                            detail: "Lower values are more sensitive. These are floors — whiz "
                                  + "measures the room at the start of each session and raises "
                                  + "them if it is noisy.\n\nIf you have to raise your voice, "
                                  + "check the system input volume first (System Settings → "
                                  + "Sound → Input); a quiet mic loses detail that no setting "
                                  + "here can recover.")
            }

            Section {
                Button("Restore Defaults") {
                    controller.updateConfig { config in
                        let defaults = WhizConfig()
                        config.frameEnergy = defaults.frameEnergy
                        config.minEnergy = defaults.minEnergy
                        config.minUtterance = defaults.minUtterance
                    }
                }
            }
        }
        .formStyle(.grouped)
    }

    private func slider(
        _ label: String,
        _ value: Binding<Double>,
        range: ClosedRange<Double>,
        digits: Int,
        unit: String = ""
    ) -> some View {
        LabeledContent(label) {
            HStack {
                Slider(value: value, in: range)
                Text(String(format: "%.\(digits)f\(unit)", value.wrappedValue))
                    .monospacedDigit()
                    .frame(width: 56, alignment: .trailing)
            }
        }
    }

    /// Writes through to the config file on every edit.
    private func binding<V>(_ path: WritableKeyPath<WhizConfig, V>) -> Binding<V> {
        Binding(
            get: { controller.config[keyPath: path] },
            set: { newValue in controller.updateConfig { $0[keyPath: path] = newValue } }
        )
    }
}

#Preview("Settings") {
    SettingsView(controller: .preview(state: .idle))
}
