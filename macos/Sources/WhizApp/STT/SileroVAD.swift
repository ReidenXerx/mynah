import CWhisper
import Foundation

/// Voice activity detection via Silero, using whisper.cpp's bundled VAD.
///
/// The energy gates in `UtteranceDetector` decide *when* an utterance starts and
/// ends — they are cheap enough to run on every audio buffer. They are also
/// crude: they only measure loudness, so a fan, a door, or a keyboard clack
/// passes just as readily as a voice. That is what feeds Whisper the near-silent
/// noise it turns into subtitle-credit hallucinations.
///
/// Silero is a small (0.8 MB) neural net that answers the actual question —
/// "is this a human voice?" — from the shape of the sound rather than its
/// volume. So the split is:
///
///   - `UtteranceDetector` (energy):  *when* does an utterance begin and end
///   - `SileroVAD` (model):           *does* it contain speech at all
///
/// Being level-independent also makes it the right pairing for Apple's voice
/// processing / AGC, which deliberately destroys the stable relationship between
/// loudness and speech that the energy gates depend on.
///
/// The model is the same one the batch pipeline downloads via
/// `whiz models download-vad` — no second asset to manage.
actor SileroVAD {

    private var context: OpaquePointer?
    private let modelURL: URL

    /// Speech probability above which a frame counts as voice. Silero's own
    /// default; raise it if noise is getting through, lower it if quiet speech
    /// is being dropped.
    static let defaultThreshold: Float = 0.5

    init(modelURL: URL) {
        self.modelURL = modelURL
    }

    var isLoaded: Bool { context != nil }

    func load() throws {
        guard context == nil else { return }
        // Required before any whisper_*_init_* call; see GGMLBackends.
        GGMLBackends.registerOnce()

        var params = whisper_vad_default_context_params()
        params.n_threads = Int32(max(1, ProcessInfo.processInfo.activeProcessorCount - 2))
        // CPU only. The model is tiny and runs in well under a millisecond;
        // dispatching it to the GPU would cost more in setup than it saves, and
        // it would contend with Whisper for the same device.
        params.use_gpu = false

        guard let ctx = whisper_vad_init_from_file_with_params(modelURL.path, params) else {
            throw WhisperError.modelLoadFailed(modelURL.lastPathComponent)
        }
        context = ctx
    }

    func unload() {
        guard let context else { return }
        whisper_vad_free(context)
        self.context = nil
    }

    /// Whether `samples` (16 kHz mono) contain any speech.
    ///
    /// Returns `true` when the detector is unavailable: a VAD that cannot run
    /// should not silently swallow every utterance. Failing open means the
    /// worst case is the old behaviour, not a mute app.
    func containsSpeech(_ samples: [Float], threshold: Float = defaultThreshold) -> Bool {
        guard let context, !samples.isEmpty else { return true }

        var params = whisper_vad_default_params()
        params.threshold = threshold
        // The utterance has already been segmented by the energy gates, so this
        // is a yes/no question about one clip — the durations only need to be
        // permissive enough not to discard a short word.
        params.min_speech_duration_ms = 60
        params.min_silence_duration_ms = 100
        params.speech_pad_ms = 0

        guard let segments = samples.withUnsafeBufferPointer({ buffer in
            whisper_vad_segments_from_samples(
                context, params, buffer.baseAddress, Int32(buffer.count))
        }) else {
            return true  // fail open
        }
        defer { whisper_vad_free_segments(segments) }

        return whisper_vad_segments_n_segments(segments) > 0
    }
}
