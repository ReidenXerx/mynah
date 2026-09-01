import CWhisper
import Foundation

/// The languages this whisper build actually supports.
///
/// Read from the library rather than hardcoded, so the list cannot drift from
/// the engine when the pinned submodule is bumped. Replaces a free-text field
/// that accepted anything: `xx`, `eng`, `РУ` and empty were all taken without
/// complaint, and whisper silently fell back instead of erroring — so a typo
/// produced quietly degraded transcription with no visible cause.
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

    /// Auto-detect first, then every language whisper knows, alphabetically.
    static let all: [Language] = {
        var out = [Language(code: autoCode, name: "Auto-detect")]
        let maxID = Int(whisper_lang_max_id())
        var known: [Language] = []
        for id in 0...maxID {
            guard let code = whisper_lang_str(Int32(id)),
                  let full = whisper_lang_str_full(Int32(id)) else { continue }
            known.append(Language(
                code: String(cString: code),
                name: String(cString: full).capitalized))
        }
        out.append(contentsOf: known.sorted { $0.name < $1.name })
        return out
    }()

    /// Whether whisper recognises `code`. Used to decide if a value carried
    /// over from a hand-edited config should be shown as-is rather than
    /// silently replaced.
    static func isKnown(_ code: String) -> Bool {
        code == autoCode || whisper_lang_id(code) >= 0
    }

    /// The entry for `code`, inventing one for an unrecognised value so a
    /// hand-edited config is displayed rather than reset behind the user's back.
    static func language(for code: String) -> Language {
        if let match = all.first(where: { $0.code == code }) { return match }
        return Language(code: code, name: code.isEmpty ? "(unset)" : "Unknown")
    }
}
