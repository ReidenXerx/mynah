import AVFoundation
import Foundation

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

    /// Called when a configuration change (device switch, Bluetooth
    /// connect/disconnect) invalidates the running stream. The owner decides
    /// whether to rebuild or end the session — capture itself has no opinion.
    private var onConfigurationChange: (@Sendable () -> Void)?

    private var configurationObserver: NSObjectProtocol?

    /// Consecutive failed conversions, for throttled error logging. Touched
    /// only from the tap's thread while running (and reset from `start`),
    /// matching this class's existing `@unchecked Sendable` threading pattern.
    private var consecutiveConversionFailures = 0

    deinit {
        if let configurationObserver {
            NotificationCenter.default.removeObserver(configurationObserver)
        }
    }

    func start(
        onFrame: @escaping @Sendable ([Float], Double) -> Void,
        onConfigurationChange: (@Sendable () -> Void)? = nil
    ) throws {
        guard !isRunning else { return }
        self.onFrame = onFrame
        self.onConfigurationChange = onConfigurationChange
        consecutiveConversionFailures = 0

        try installTap()
        do {
            try engine.start()
        } catch {
            // The tap must not outlive the engine it was installed on. A
            // leaked tap here meant the next `start()` hit
            // `installTap(onBus: 0)` with a tap already present, and
            // AVAudioEngine raises an ObjC exception on that — so one failed
            // start cascaded into a crash on the *next* session start (H3).
            // `stop()` is also unconditional for the same reason: its old
            // `isRunning` guard made it a no-op exactly when cleanup was
            // needed most.
            Log.audio.error(
                "engine start failed — removing tap: \(error.localizedDescription, privacy: .public)")
            stop()
            throw error
        }
        isRunning = true
        observeConfigurationChanges()
    }

    /// Unconditional, not guarded on `isRunning`: a failed start leaves a tap
    /// installed but `isRunning == false`, and a guarded `stop()` could never
    /// remove it. `removeTap` on a bus without a tap is a documented no-op, so
    /// an extra call is harmless.
    func stop() {
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        converter = nil
        onFrame = nil
        onConfigurationChange = nil
        if let observer = configurationObserver {
            NotificationCenter.default.removeObserver(observer)
            configurationObserver = nil
        }
        isRunning = false
    }

    /// Remove the tap and rebuild the converter around the (possibly new)
    /// input format, then restart the engine. Called when a configuration
    /// change invalidates the running stream, and a no-op when idle.
    func restart() throws {
        guard isRunning, let onFrame else { return }
        let rebuild = onConfigurationChange
        Log.audio.notice("rebuilding capture stream after configuration change")
        stop()
        try start(onFrame: onFrame, onConfigurationChange: rebuild)
    }

    // MARK: - Internals

    private func installTap() throws {
        let input = engine.inputNode
        let inputFormat = input.inputFormat(forBus: 0)

        guard let targetFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: WhisperEngine.sampleRate,
            channels: 1,
            interleaved: false
        ) else { throw AudioCaptureError.unsupportedFormat }

        // A converter built from the *old* input format after a device change
        // silently produces nothing — this is why restart() rebuilds it rather
        // than reusing the instance.
        converter = AVAudioConverter(from: inputFormat, to: targetFormat)

        input.installTap(onBus: 0, bufferSize: 2048, format: inputFormat) { [weak self] buffer, _ in
            guard let self, let converted = self.convert(buffer, to: targetFormat) else {
                self?.noteConversionFailure()
                return
            }
            self.consecutiveConversionFailures = 0
            let level = min(1.0, TranscriptFilter.rms(converted) * 8)
            self.onFrame?(converted, level)
        }
    }

    /// Surface a conversion failure rather than silently dropping frames (M6):
    /// a converter-nil or converter-error used to be indistinguishable from
    /// "the user said nothing". Logged on the first failure and then rarely, so
    /// a persistent fault is visible without flooding the log at 30+ Hz.
    private func noteConversionFailure() {
        consecutiveConversionFailures += 1
        if consecutiveConversionFailures == 1 || consecutiveConversionFailures % 500 == 0 {
            Log.audio.error(
                "audio conversion failed (\(self.consecutiveConversionFailures) consecutive) — input device may have changed; try restarting the session")
        }
    }

    /// Rebuild the tap + converter when the audio graph changes (default
    /// device switch, Bluetooth connect/disconnect). Without this the old
    /// converter kept running against a dead stream — the app looked alive
    /// but produced nothing (M6).
    private func observeConfigurationChanges() {
        // One observer at a time; start() while already running is a no-op, so
        // this only lands on the first start of a session.
        if configurationObserver != nil { return }
        configurationObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange,
            object: engine,
            queue: nil
        ) { [weak self] _ in
            // Documented to arrive on an arbitrary queue; hop to the owner,
            // which is main-actor isolated via its own task hop.
            self?.onConfigurationChange?()
        }
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
