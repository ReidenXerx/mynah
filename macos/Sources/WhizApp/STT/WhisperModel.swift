import Foundation

/// Locates the ggml model file for dictation.
///
/// Deliberately reuses the ggml models the batch pipeline already downloads
/// (`whiz models download`), searching the same directories as
/// `DEFAULT_MODEL_SEARCH_DIRS` in `whiz/config.py`. Before this, dictation kept
/// a *second* model in a *second* format — mlx safetensors under
/// `~/.cache/huggingface`, 1.6 GB, entirely separate from the ggml `.bin` files.
/// One engine now means one model file.
enum WhisperModel {

    /// Preference order, aligned with `models.py:PREFERENCE` by NS-15.
    ///
    /// Quantization corrupts transcription quality — commit ea49da8 recorded
    /// 4-bit turbo producing "garbled mixed-language output on real speech"
    /// (turbo's 4 decoder layers suffer most), and the user decision extends
    /// it to every quantized variant. Unquantized models come first, every
    /// `-q*` variant sits behind its unquantized class, and a quantized model
    /// resolves only when nothing unquantized exists on disk.
    static let preference = [
        "ggml-large-v3-turbo.bin",
        "ggml-large-v3.bin",
        "ggml-medium.bin",
        "ggml-large-v3-turbo-q8_0.bin",
        "ggml-large-v3-turbo-q5_0.bin",
        "ggml-large-v3-q5_0.bin",
    ]

    /// Mirrors `DEFAULT_MODEL_SEARCH_DIRS` in `whiz/config.py`.
    static var searchDirectories: [URL] {
        let home = FileManager.default.homeDirectoryForCurrentUser
        return [
            home.appendingPathComponent(".cache/whisper"),
            home.appendingPathComponent(
                "Library/Application Support/com.unspoken.app/WhisperModels"),
            home.appendingPathComponent("Library/Caches/whisper"),
            URL(fileURLWithPath: "/usr/local/share/whisper"),
            URL(fileURLWithPath: "/opt/homebrew/share/whisper"),
            URL(fileURLWithPath: "/usr/share/whisper"),
        ]
    }

    /// Resolve the model to load. An explicit `configured` path wins; otherwise
    /// walk the preference order across every search directory.
    static func resolve(configured: String) -> URL? {
        if !configured.isEmpty {
            let expanded = (configured as NSString).expandingTildeInPath
            let url = URL(fileURLWithPath: expanded)
            if FileManager.default.fileExists(atPath: url.path) { return url }
            // A bare filename in config, rather than a full path.
            for directory in searchDirectories {
                let candidate = directory.appendingPathComponent(expanded)
                if FileManager.default.fileExists(atPath: candidate.path) { return candidate }
            }
            return nil
        }
        for name in preference {
            for directory in searchDirectories {
                let candidate = directory.appendingPathComponent(name)
                if FileManager.default.fileExists(atPath: candidate.path) { return candidate }
            }
        }
        return nil
    }

    /// The Silero VAD model, downloaded by `whiz models download-vad`.
    /// `VAD_MODELS` in `whiz/models.py` lists v5.1.2 first for whisper-cli
    /// compatibility, so match that ordering.
    static func resolveVAD() -> URL? {
        for name in ["ggml-silero-v5.1.2.bin", "ggml-silero-v6.2.0.bin"] {
            for directory in searchDirectories {
                let candidate = directory.appendingPathComponent(name)
                if FileManager.default.fileExists(atPath: candidate.path) { return candidate }
            }
        }
        return nil
    }
}
