import Foundation
import Observation

/// Downloads ggml models from HuggingFace.
///
/// Removes the last reason a user needs the Python CLI installed: without this,
/// a fresh install can do nothing until someone runs `whiz models download`,
/// which means installing pipx and the whole package just to fetch two files.
///
/// Sources and destination deliberately match `whiz/models.py`, so a model
/// fetched by either side is found by both — one cache, not two.
@MainActor
final class ModelDownloader: ObservableObject {

    /// Same repositories `whiz/models.py` uses.
    private static let whisperBase = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main"
    private static let vadBase = "https://huggingface.co/ggml-org/whisper-vad/resolve/main"

    /// Same destination as the Python CLI's default, and first in
    /// `WhisperModel.searchDirectories`.
    static var destination: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".cache/whisper")
    }

    struct Option: Identifiable, Hashable {
        var id: String { filename }
        var filename: String
        var label: String
        var detail: String
        var approximateBytes: Int64
    }

    /// What we offer, and why this order.
    ///
    /// Unquantized turbo is first because commit ea49da8 found 4-bit turbo
    /// produced "garbled mixed-language output on real speech" — turbo has only
    /// four decoder layers, so quantization costs it more than it costs full
    /// large. q5_0 is milder than q4 and is offered for people short on disk,
    /// but it is labelled honestly rather than presented as equivalent.
    static let options: [Option] = [
        Option(filename: "ggml-large-v3-turbo.bin",
               label: "Large v3 Turbo (recommended)",
               detail: "Best accuracy for non-English speech.",
               approximateBytes: 1_620_000_000),
        Option(filename: "ggml-large-v3-turbo-q5_0.bin",
               label: "Large v3 Turbo, quantized",
               detail: "A third of the size. Quantization degrades non-English "
                     + "accuracy — not validated for Russian.",
               approximateBytes: 574_000_000),
    ]

    enum State: Equatable {
        case idle
        case downloading(progress: Double, received: Int64, total: Int64)
        case failed(String)
        case finished(String)
    }

    @Published private(set) var state: State = .idle

    private var task: URLSessionDownloadTask?
    private var delegate: DownloadDelegate?

    var isDownloading: Bool {
        if case .downloading = state { return true }
        return false
    }

    // MARK: - Public

    func downloadModel(_ option: Option) {
        start(url: "\(Self.whisperBase)/\(option.filename)", filename: option.filename)
    }

    /// The VAD model is under a megabyte, so it is fetched without ceremony.
    func downloadVAD() {
        start(url: "\(Self.vadBase)/ggml-silero-v5.1.2.bin",
              filename: "ggml-silero-v5.1.2.bin")
    }

    func cancel() {
        task?.cancel()
        task = nil
        state = .idle
    }

    // MARK: - Internal

    private func start(url urlString: String, filename: String) {
        guard !isDownloading, let url = URL(string: urlString) else { return }
        state = .downloading(progress: 0, received: 0, total: 0)

        let destination = Self.destination.appendingPathComponent(filename)
        let delegate = DownloadDelegate(
            destination: destination,
            onProgress: { [weak self] received, total in
                Task { @MainActor in
                    guard let self, self.isDownloading else { return }
                    let progress = total > 0 ? Double(received) / Double(total) : 0
                    self.state = .downloading(progress: progress, received: received, total: total)
                }
            },
            onFinish: { [weak self] result in
                Task { @MainActor in
                    guard let self else { return }
                    self.task = nil
                    switch result {
                    case .success:
                        Log.stt.notice("downloaded \(filename, privacy: .public)")
                        self.state = .finished(filename)
                    case .failure(let error):
                        Log.stt.error(
                            "download failed: \(error.localizedDescription, privacy: .public)")
                        self.state = .failed(error.localizedDescription)
                    }
                }
            })
        self.delegate = delegate

        // HuggingFace redirects to a CDN, which URLSession follows by default.
        let session = URLSession(configuration: .default, delegate: delegate, delegateQueue: nil)
        let task = session.downloadTask(with: url)
        self.task = task
        task.resume()
    }
}

/// Moves the finished file into place and reports progress.
///
/// A delegate rather than the closure-based API because only the delegate form
/// reports incremental progress, and a 1.6 GB download with no progress bar
/// looks identical to one that has hung.
private final class DownloadDelegate: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {

    private let destination: URL
    private let onProgress: @Sendable (Int64, Int64) -> Void
    private let onFinish: @Sendable (Result<Void, Error>) -> Void

    init(
        destination: URL,
        onProgress: @escaping @Sendable (Int64, Int64) -> Void,
        onFinish: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        self.destination = destination
        self.onProgress = onProgress
        self.onFinish = onFinish
    }

    func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didWriteData bytesWritten: Int64,
        totalBytesWritten: Int64,
        totalBytesExpectedToWrite: Int64
    ) {
        onProgress(totalBytesWritten, totalBytesExpectedToWrite)
    }

    func urlSession(
        _ session: URLSession,
        downloadTask: URLSessionDownloadTask,
        didFinishDownloadingTo location: URL
    ) {
        // Reject an HTTP error page saved as if it were a model: a 404 body is
        // a perfectly valid file, and without this check it lands on disk named
        // ggml-large-v3-turbo.bin and fails much later with a confusing error.
        if let response = downloadTask.response as? HTTPURLResponse,
           !(200..<300).contains(response.statusCode) {
            onFinish(.failure(DownloadError.http(response.statusCode)))
            return
        }

        do {
            try FileManager.default.createDirectory(
                at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
            // The temporary file vanishes when this method returns, so move it now.
            try? FileManager.default.removeItem(at: destination)
            try FileManager.default.moveItem(at: location, to: destination)
            onFinish(.success(()))
        } catch {
            onFinish(.failure(error))
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error, (error as NSError).code != NSURLErrorCancelled {
            onFinish(.failure(error))
        }
    }
}

enum DownloadError: LocalizedError {
    case http(Int)

    var errorDescription: String? {
        switch self {
        case .http(let code): return "Server returned HTTP \(code)."
        }
    }
}
