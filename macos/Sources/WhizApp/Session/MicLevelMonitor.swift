import AVFoundation

/// Microphone amplitude monitor — level metering only, no capture buffer.
///
/// This exists so the indicator has something real to animate before speech
/// recognition lands. It taps the input node, computes RMS per buffer, and
/// throws the samples away.
///
/// It is **not** the capture path. When STT is wired up, the utterance pipeline
/// from `engine.py` (ring buffer → VAD → energy gate → utterance queue) belongs
/// here, and this becomes a tap on that stream rather than its own engine.
final class MicLevelMonitor: @unchecked Sendable {

    private let engine = AVAudioEngine()
    private var isRunning = false

    /// Start metering. `onLevel` is called from the audio thread with an
    /// amplitude in 0...1 — hop to the main actor before touching UI state.
    func start(onLevel: @escaping @Sendable (Double) -> Void) {
        guard !isRunning else { return }

        let input = engine.inputNode
        let format = input.inputFormat(forBus: 0)

        // A large buffer keeps the callback rate low; we only need enough
        // resolution for a waveform animation, not for segmentation.
        input.installTap(onBus: 0, bufferSize: 2048, format: format) { buffer, _ in
            guard let channel = buffer.floatChannelData?[0] else { return }
            let count = Int(buffer.frameLength)
            guard count > 0 else { return }

            var sum: Float = 0
            for i in 0..<count { sum += channel[i] * channel[i] }
            let rms = (sum / Float(count)).squareRoot()

            // Speech sits well below full scale, so scale up before clamping —
            // otherwise the bars barely move at normal speaking volume.
            onLevel(min(1.0, Double(rms) * 8))
        }

        do {
            try engine.start()
            isRunning = true
        } catch {
            input.removeTap(onBus: 0)
            NSLog("whiz: could not start audio engine: \(error.localizedDescription)")
        }
    }

    func stop() {
        guard isRunning else { return }
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        isRunning = false
    }
}
