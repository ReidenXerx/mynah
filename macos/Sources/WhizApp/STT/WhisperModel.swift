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
    /// Grouped per class: the unquantized model first, then that class's
    /// quantized variants best-quality-first (q8_0 before q5_0). A quantized
    /// model resolves only when its own unquantized class is absent from
    /// disk — quantization corrupts transcription quality (commit ea49da8
    /// recorded 4-bit turbo producing "garbled mixed-language output on real
    /// speech"), and a global unquantized-first batch let `tiny` outrank
    /// `large-v3-turbo-q8_0`, which is never acceptable. `tiny` is excluded
    /// entirely (useless quality); it still loads if configured explicitly.
    /// Pinned by `WhisperModelTests.swift`.
    static let preference = [
        "ggml-large-v3-turbo.bin",
        "ggml-large-v3-turbo-q8_0.bin",
        "ggml-large-v3-turbo-q5_0.bin",
        "ggml-large-v3.bin",
        "ggml-large-v3-q5_0.bin",
        "ggml-medium.bin",
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
    /// walk the preference order across every search directory. `searchDirs`
    /// lets the tests point resolution at a temp directory instead of mutating
    /// static state (which would race under swift-testing's parallel runs);
    /// production callers use the default.
    static func resolve(configured: String, searchDirs: [URL] = searchDirectories) -> URL? {
        if !configured.isEmpty {
            let expanded = (configured as NSString).expandingTildeInPath
            let url = URL(fileURLWithPath: expanded)
            if FileManager.default.fileExists(atPath: url.path) { return url }
            // A bare filename in config, rather than a full path.
            for directory in searchDirs {
                let candidate = directory.appendingPathComponent(expanded)
                if FileManager.default.fileExists(atPath: candidate.path) { return candidate }
            }
            return nil
        }
        for name in preference {
            for directory in searchDirs {
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
