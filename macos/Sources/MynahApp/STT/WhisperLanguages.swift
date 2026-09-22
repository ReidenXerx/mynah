import CMynah
enum WhisperLanguages {

    struct Language: Identifiable, Hashable {
        var code: String
        var name: String
        var id: String { code }
        var label: String { code == autoCode ? name : "\(name) (\(code))" }
    }

    /// whisper.cpp treats this as "detect the language", so it is not in the
    /// numeric table and has to be added by hand.
    static let autoCode = "auto"

    /// Auto-detect first, then every language the engine knows, alphabetically.
    /// Read through the C API (mynah_language_*), so the list cannot drift
    /// from the engine when the pinned submodule is bumped.
    static let all: [Language] = {
        var out = [Language(code: autoCode, name: "Auto-detect")]
        let count = mynah_language_count()
        var known: [Language] = []
        for id in 0..<count {
            guard let code = mynah_language_code(id),
                  let full = mynah_language_name(id) else { continue }
            known.append(Language(
                code: String(cString: code),
                name: String(cString: full).capitalized))
        }
        out.append(contentsOf: known.sorted { $0.name < $1.name })
        return out
    }()

    /// Whether the engine recognises `code`. Used to decide if a value carried
    /// over from a hand-edited config should be shown as-is rather than
    /// silently replaced.
    static func isKnown(_ code: String) -> Bool {
        code == autoCode || mynah_language_id(code) >= 0
    }

    /// The entry for `code`, inventing one for an unrecognised value so a
    /// hand-edited config is displayed rather than reset behind the user's back.
    static func language(for code: String) -> Language {
        if let match = all.first(where: { $0.code == code }) { return match }
        return Language(code: code, name: code.isEmpty ? "(unset)" : "Unknown")
    }
}
