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
    ///
    /// All five NS-15 classes (large-v3-turbo > large-v3 > medium > small >
    /// base), matching the current `models.py:PREFERENCE` entry for entry —
    /// not just the historical first three. The list stopped at `medium`
    /// while Python's grew to eleven entries, so any disk holding only
    /// small/base models auto-picked nothing here and picked a model on the
    /// Python side; prefix alias resolution below walks the same list, so
    /// the two halves of that divergence share one fix.
    static let preference = [
        "ggml-large-v3-turbo.bin",
        "ggml-large-v3-turbo-q8_0.bin",
        "ggml-large-v3-turbo-q5_0.bin",
        "ggml-large-v3.bin",
        "ggml-large-v3-q5_0.bin",
        "ggml-medium.bin",
        "ggml-medium-q5_0.bin",
        "ggml-small.bin",
        "ggml-small-q5_0.bin",
        "ggml-base.bin",
        "ggml-base-q5_0.bin",
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

    /// Resolve the model to load. `searchDirs` lets the tests point
    /// resolution at a temp directory instead of mutating static state (which
    /// would race under swift-testing's parallel runs); production callers
    /// use the default.
    ///
    /// An explicit `configured` value wins in this order:
    ///
    ///   1. an existing file path (absolute or `~`-relative)
    ///   2. a bare filename present in a search directory
    ///   3. an alias, resolved exactly as `resolve` in `whiz/models.py` does,
    ///      so the same `dictate_model` value means the same model from
    ///      either side:
    ///        - full alias  `large-v3-turbo-q5_0`
    ///        - short alias `turbo` (only when it matches exactly one model,
    ///      as in Python — ambiguity resolves to nothing)
    ///        - prefix      `large-v3`, picking the best on-disk variant by
    ///      preference order
    ///
    /// Before the alias stage existed, anything but a literal path or
    /// filename returned nil: `dictate_model = "turbo"` (a name the Python
    /// CLI happily resolves) failed with "no model found" here, and
    /// `large-v3` on a disk holding only `ggml-large-v3-q5_0.bin` worked in
    /// Python and not in Swift. One alias form is deliberately MORE
    /// forgiving than Python: `ggml-medium.bin` normalizes to the alias
    /// `medium`, because the Swift settings UI displays filenames in that
    /// exact form and a user may copy one back into the config.
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
            return resolveAlias(configured, searchDirs: searchDirs)
        }
        for name in preference {
            for directory in searchDirs {
                let candidate = directory.appendingPathComponent(name)
                if FileManager.default.fileExists(atPath: candidate.path) { return candidate }
            }
        }
        return nil
    }

    /// A model found on disk, keyed by the alias Python would give it.
    private struct DiscoveredModel {
        var alias: String
        var url: URL
    }

    /// Alias resolution, mirroring `resolve` in `whiz/models.py` once the
    /// path and bare-filename checks above have missed.
    private static func resolveAlias(_ name: String, searchDirs: [URL]) -> URL? {
        let found = discover(searchDirs: searchDirs)
        guard !found.isEmpty else { return nil }
        let aliases = Dictionary(
            uniqueKeysWithValues: found.map { ($0.alias, $0.url) })

        // Accept "ggml-<alias>.bin" as an alias (see the resolve doc above).
        let wanted = alias(fromFilename: name)

        // Exact alias match.
        if let url = aliases[wanted] { return url }

        // Short alias: "turbo" collapses every turbo variant. Python only
        // resolves on a UNIQUE match — with both turbo variants on disk,
        // "turbo" is ambiguous and resolves to nothing, exactly as here.
        let shortMatches = found.filter { shortAlias($0.alias) == shortAlias(wanted) }
        if shortMatches.count == 1 { return shortMatches[0].url }

        // Prefix match: "large-v3" matches "large-v3-q5_0" (and every
        // turbo variant). Pick by preference order, else the first found.
        let prefixed = found.filter { $0.alias.hasPrefix(wanted) }
        if !prefixed.isEmpty {
            let prefixedAliases = Set(prefixed.map(\.alias))
            for candidate in preference where prefixedAliases.contains(alias(fromFilename: candidate)) {
                return aliases[alias(fromFilename: candidate)]
            }
            return prefixed[0].url
        }
        return nil
    }

    /// Mirror of `discover` in `whiz/models.py`: scan the search directories
    /// for `ggml-*.bin`, first directory wins per alias, sorted by alias.
    private static func discover(searchDirs: [URL]) -> [DiscoveredModel] {
        var found: [String: URL] = [:]
        let fileManager = FileManager.default
        for directory in searchDirs {
            guard let contents = try? fileManager.contentsOfDirectory(
                at: directory, includingPropertiesForKeys: [.isRegularFileKey]) else { continue }
            for file in contents {
                let name = file.lastPathComponent
                guard name.hasPrefix("ggml-"), name.hasSuffix(".bin") else { continue }
                let isFile = (try? file.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile
                guard isFile == true else { continue }
                let modelAlias = alias(fromFilename: name)
                if found[modelAlias] == nil { found[modelAlias] = file }
            }
        }
        return found
            .map { DiscoveredModel(alias: $0.key, url: $0.value) }
            .sorted { $0.alias < $1.alias }
    }

    /// `ggml-large-v3-turbo-q5_0.bin` → `large-v3-turbo-q5_0`
    /// (mirror of `_alias_from_name`).
    static func alias(fromFilename name: String) -> String {
        var base = name
        if base.hasPrefix("ggml-") { base.removeFirst("ggml-".count) }
        if base.hasSuffix(".bin") { base.removeLast(".bin".count) }
        return base
    }

    /// `large-v3-turbo-q5_0` → `turbo`; anything else maps to itself
    /// (mirror of `_short_alias`).
    private static func shortAlias(_ alias: String) -> String {
        alias.contains("turbo") ? "turbo" : alias
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
