import CWhisper
import Foundation

/// Speech recognition via whisper.cpp, linked directly through its C API.
///
/// Replaces `providers/mlx.py`. The engine choice is recorded in
/// `docs/SWIFT-APP.md`; the short version is that whisper.cpp runs the same
/// OpenAI weights the mlx provider did, keeps `initial_prompt` (which the
/// Russian anti-censorship prompt depends on), is already a whiz dependency for
/// batch transcription, and is not Apple-Silicon-only.
///
/// Linked in-process, not spawned per utterance. `whisper-cli` as a subprocess
/// would reload the model every time — the mlx provider kept the model warm
/// between utterances and so must this.
///
/// An `actor` because the whisper context is not thread-safe and transcription
/// runs off the main thread. Actor isolation serialises access without a manual
/// lock, and the compiler enforces it — `engine.py` had to arrange the same
/// guarantee by convention, with a comment asking future readers to respect it.
actor WhisperEngine {

    private var context: OpaquePointer?
    private let modelURL: URL

    /// Whisper is trained on 16 kHz mono.
    static let sampleRate: Double = 16_000

    init(modelURL: URL) {
        self.modelURL = modelURL
    }

    var isLoaded: Bool { context != nil }

    // MARK: - Lifecycle

    /// Load the model. Seconds on a cold start, which is why the caller keeps it
    /// resident for `dictate_idle_timeout` after a session rather than unloading
    /// immediately.
    func load() throws {
        guard context == nil else { return }

        GGMLBackends.registerOnce()

        var params = whisper_context_default_params()
        // Metal. The whole reason for choosing a GPU-capable runtime.
        params.use_gpu = true
        params.flash_attn = true

        guard let ctx = whisper_init_from_file_with_params(modelURL.path, params) else {
            throw WhisperError.modelLoadFailed(modelURL.lastPathComponent)
        }
        context = ctx
    }

    /// Free the model — the "zero RAM at idle" behaviour from `engine.py`.
    ///
    /// Must be called explicitly; an actor's `deinit` cannot touch the
    /// non-Sendable context, so there is no automatic cleanup fallback.
    func unload() {
        guard let context else { return }
        whisper_free(context)
        self.context = nil
    }

    // MARK: - Transcription

    /// Transcribe mono 16 kHz float samples, returning recognised text.
    ///
    /// `prompt` biases the decoder — this is what carries
    /// `DEFAULT_RUSSIAN_PROMPT` across from the Python engine, and it is
    /// load-bearing: without it Whisper sanitises Russian slang and obscenity
    /// rather than transcribing it verbatim.
    func transcribe(
        samples: [Float],
        language: String,
        prompt: String,
        threads: Int = max(1, ProcessInfo.processInfo.activeProcessorCount - 2)
    ) throws -> String {
        guard let context else { throw WhisperError.notLoaded }
        guard !samples.isEmpty else { return "" }

        var params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY)
        params.n_threads = Int32(threads)
        params.print_progress = false
        params.print_realtime = false
        params.print_timestamps = false
        params.print_special = false
        params.translate = false
        // Utterances are segmented before they get here, so each call is one
        // self-contained chunk; carrying decoder context across them lets a
        // hallucination in one utterance seed the next.
        params.no_context = true
        params.no_timestamps = true
        params.suppress_blank = true
        // Suppress non-speech tokens: [MUSIC], [SOUND] and similar. A cheap
        // extra layer under the hallucination filter.
        params.suppress_nst = true

        // `whisper_full` does not copy these strings, so they must outlive the
        // call. Holding them in Swift `String`s and passing pointers into a
        // nested `withCString` keeps them alive for exactly the right scope.
        return try language.withCString { languagePtr in
            try prompt.withCString { promptPtr in
                params.language = languagePtr
                params.initial_prompt = prompt.isEmpty ? nil : promptPtr

                let status = samples.withUnsafeBufferPointer { buffer in
                    whisper_full(context, params, buffer.baseAddress, Int32(buffer.count))
                }
                guard status == 0 else { throw WhisperError.transcriptionFailed(Int(status)) }

                var text = ""
                for index in 0..<whisper_full_n_segments(context) {
                    if let segment = whisper_full_get_segment_text(context, index) {
                        text += String(cString: segment)
                    }
                }
                return text.trimmingCharacters(in: .whitespacesAndNewlines)
            }
        }
    }
}

enum WhisperError: LocalizedError {
    case notLoaded
    case modelLoadFailed(String)
    case transcriptionFailed(Int)
    case noModelFound

    var errorDescription: String? {
        switch self {
        case .notLoaded:
            return "Whisper model is not loaded."
        case .modelLoadFailed(let name):
            return "Could not load Whisper model '\(name)'."
        case .transcriptionFailed(let code):
            return "Whisper transcription failed (status \(code))."
        case .noModelFound:
            return """
                No Whisper model found. Download one with:
                  whiz models download ggml-large-v3-turbo.bin
                """
        }
    }
}
