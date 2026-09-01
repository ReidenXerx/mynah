import SwiftUI

/// Model status and download controls for the Recognition settings tab.
///
/// Before this the app could only tell the user to go and run
/// `whiz models download` — which meant installing Python, pipx and the whole
/// whiz package to fetch two files. That was the last hard dependency on the
/// Python CLI for someone who only wants dictation.
struct ModelSectionView: View {
    @ObservedObject var controller: SessionController
    @StateObject private var downloader = ModelDownloader()

    @State private var selection: ModelDownloader.Option = ModelDownloader.options[0]
    /// Bumped after a download so the "is it on disk?" checks re-run — the
    /// filesystem does not notify us.
    @State private var refresh = 0

    var body: some View {
        Section("Speech model") {
            if let model = installedModel {
                LabeledContent("Installed") {
                    Text(model).foregroundStyle(.secondary)
                }
            } else {
                Picker("Model", selection: $selection) {
                    ForEach(ModelDownloader.options) { option in
                        Text(option.label).tag(option)
                    }
                }
                Text(selection.detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            downloadControls
        }

        Section("Voice activity detection") {
            Toggle("Reject non-speech audio", isOn: vadBinding)
                .disabled(!hasVAD)
            if hasVAD {
                AppliesNote(.nextSession,
                            detail: "Uses Silero to check an utterance is a human voice, not "
                                  + "just loud. Rejects fans, keyboards and vacuum cleaners "
                                  + "before transcription.")
            } else {
                HStack {
                    Text("Silero model not installed (0.8 MB).")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("Download") { downloader.downloadVAD() }
                        .disabled(downloader.isDownloading)
                }
            }
        }
    }

    // MARK: - Download UI

    @ViewBuilder
    private var downloadControls: some View {
        switch downloader.state {
        case .downloading(let progress, let received, let total):
            VStack(alignment: .leading, spacing: 6) {
                ProgressView(value: progress)
                HStack {
                    Text(total > 0
                         ? "\(format(received)) of \(format(total))"
                         : "Starting…")
                        .font(.caption)
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("Cancel") { downloader.cancel() }
                }
            }

        case .failed(let message):
            VStack(alignment: .leading, spacing: 6) {
                Text(message).font(.caption).foregroundStyle(.red)
                Button("Try Again") { downloader.downloadModel(selection) }
            }

        case .finished, .idle:
            if installedModel == nil {
                HStack {
                    Text("About \(format(selection.approximateBytes)) to download.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("Download") { downloader.downloadModel(selection) }
                }
            }
        }
    }

    // MARK: - State

    private var installedModel: String? {
        _ = refresh  // re-evaluate after a download completes
        return WhisperModel.resolve(configured: controller.config.model)?.lastPathComponent
    }

    private var hasVAD: Bool {
        _ = refresh
        return WhisperModel.resolveVAD() != nil
    }

    private var vadBinding: Binding<Bool> {
        Binding(
            get: { controller.config.vad },
            set: { value in controller.updateConfig { $0.vad = value } }
        )
    }

    private func format(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        return formatter.string(fromByteCount: bytes)
    }
}
