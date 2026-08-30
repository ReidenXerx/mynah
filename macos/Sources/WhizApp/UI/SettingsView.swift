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
                    TextField("", text: binding(\.hotkey))
                        .frame(width: 160)
                }
                Text("pynput syntax, e.g. `<cmd>+<shift>+.` or `<f8>`. Re-registered immediately.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Toggle("Show floating indicator", isOn: binding(\.showIndicator))
                Toggle("Keep indicator visible when idle", isOn: binding(\.idleVisible))
                    .disabled(!controller.config.showIndicator)
                Text("Indicator changes apply after restarting whiz.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
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
                Text("Keeps the model warm for back-to-back dictation, then frees the memory.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }

    // MARK: - Recognition

    private var recognition: some View {
        Form {
            Section {
                LabeledContent("Language") {
                    TextField("", text: binding(\.language)).frame(width: 80)
                }
                LabeledContent("Model") {
                    Text(resolvedModelName).foregroundStyle(.secondary)
                }
            }

            Section {
                Toggle("Voice activity detection", isOn: binding(\.vad))
                Text(vadDescription)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Text("Prompt")
                TextEditor(text: binding(\.prompt))
                    .font(.system(.body, design: .monospaced))
                    .frame(height: 60)
                Text("Biases recognition. Leave empty for the built-in Russian prompt that "
                     + "stops Whisper censoring slang and obscenity.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }

    private var resolvedModelName: String {
        WhisperModel.resolve(configured: controller.config.model)?.lastPathComponent
            ?? "none found — run: whiz models download ggml-large-v3-turbo.bin"
    }

    private var vadDescription: String {
        WhisperModel.resolveVAD() == nil
            ? "Silero model missing — run: whiz models download-vad"
            : "Uses Silero to check an utterance is a human voice, not just loud. "
              + "Rejects fans, keyboards and vacuum cleaners before transcription."
    }

    // MARK: - Sensitivity

    private var sensitivity: some View {
        Form {
            Section {
                slider("Frame energy", binding(\.frameEnergy), range: 0.001...0.05, digits: 3)
                slider("Utterance energy", binding(\.minEnergy), range: 0.001...0.05, digits: 3)
                slider("Min utterance", binding(\.minUtterance), range: 0.05...1.0, digits: 2, unit: "s")
            } footer: {
                Text("Lower values are more sensitive. These are floors — whiz measures the "
                     + "room at the start of each session and raises them if it is noisy.\n\n"
                     + "If you have to raise your voice, check the system input volume first "
                     + "(System Settings → Sound → Input); a quiet mic loses detail that no "
                     + "setting here can recover.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
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
