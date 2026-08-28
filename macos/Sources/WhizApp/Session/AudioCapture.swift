import AVFoundation

/// Mic capture at Whisper's native 16 kHz mono.
///
/// Replaces `MicLevelMonitor`, which only metered. The hardware rarely runs at
/// 16 kHz, so an `AVAudioConverter` resamples — `engine.py` got this for free by
/// asking `sounddevice` for a 16 kHz stream, but AVAudioEngine gives you the
/// device's real format and expects you to convert.
///
/// The tap callback must never block: it hands samples off and returns. Anything
/// slow here (transcription above all) causes dropouts.
final class AudioCapture: @unchecked Sendable {

    private let engine = AVAudioEngine()
    private var converter: AVAudioConverter?
    private var isRunning = false

    /// Called on the audio thread with 16 kHz mono samples plus their RMS level.
    private var onFrame: (@Sendable ([Float], Double) -> Void)?

    func start(onFrame: @escaping @Sendable ([Float], Double) -> Void) throws {
        guard !isRunning else { return }
        self.onFrame = onFrame

        let input = engine.inputNode
        let inputFormat = input.inputFormat(forBus: 0)

        guard let targetFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: WhisperEngine.sampleRate,
            channels: 1,
            interleaved: false
        ) else { throw AudioCaptureError.unsupportedFormat }

        converter = AVAudioConverter(from: inputFormat, to: targetFormat)

        input.installTap(onBus: 0, bufferSize: 2048, format: inputFormat) { [weak self] buffer, _ in
            guard let self, let converted = self.convert(buffer, to: targetFormat) else { return }
            let level = min(1.0, TranscriptFilter.rms(converted) * 8)
            self.onFrame?(converted, level)
        }

        try engine.start()
        isRunning = true
    }

    func stop() {
        guard isRunning else { return }
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        converter = nil
        onFrame = nil
        isRunning = false
    }

    private func convert(_ buffer: AVAudioPCMBuffer, to format: AVAudioFormat) -> [Float]? {
        guard let converter else { return nil }

        let ratio = format.sampleRate / buffer.format.sampleRate
        let capacity = AVAudioFrameCount(Double(buffer.frameLength) * ratio) + 1
        guard let output = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: capacity) else {
            return nil
        }

        // AVAudioConverter's input block is typed `@Sendable`, but it is
        // invoked synchronously on this thread before `convert` returns — there
        // is no actual concurrency. Boxing the state makes that explicit
        // instead of capturing a mutable var across the annotation.
        let source = InputBox(buffer: buffer)
        var error: NSError?
        converter.convert(to: output, error: &error) { _, status in
            // The converter asks repeatedly; supply the buffer once, then
            // report end-of-stream or it will spin.
            guard let next = source.take() else {
                status.pointee = .noDataNow
                return nil
            }
            status.pointee = .haveData
            return next
        }
        guard error == nil, let channel = output.floatChannelData?[0] else { return nil }
        return Array(UnsafeBufferPointer(start: channel, count: Int(output.frameLength)))
    }
}

/// Single-use holder for the converter's input buffer. `@unchecked Sendable` is
/// sound here because the converter consumes it synchronously on one thread.
private final class InputBox: @unchecked Sendable {
    private var buffer: AVAudioPCMBuffer?

    init(buffer: AVAudioPCMBuffer) { self.buffer = buffer }

    func take() -> AVAudioPCMBuffer? {
        defer { buffer = nil }
        return buffer
    }
}

enum AudioCaptureError: LocalizedError {
    case unsupportedFormat

    var errorDescription: String? {
        "Could not create a 16 kHz mono audio format for capture."
    }
}
