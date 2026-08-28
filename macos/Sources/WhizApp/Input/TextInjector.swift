import AppKit

/// Types transcribed text into whatever app has keyboard focus.
///
/// Ported from `macos_inject.py`, keeping its central decision: split per
/// utterance, not per character. ASCII goes through synthesised keystrokes;
/// anything non-ASCII goes through the clipboard and a ⌘V, because CGEvent
/// keystrokes cannot emit arbitrary Unicode. Russian is entirely non-ASCII, so
/// in practice dictation takes the paste path.
///
/// Both paths need Accessibility. Without it CGEvent posting silently does
/// nothing — no error, no exception, just no text.
enum TextInjector {

    static func type(_ text: String) {
        guard !text.isEmpty else { return }

        // Without Accessibility, CGEvent posting does nothing at all — no
        // error, no exception, no text. That silence is the single most
        // confusing failure this app has, so name it explicitly.
        if !Permissions.isAccessibilityTrusted {
            Log.ui.error(
                "injection SKIPPED: Accessibility not granted — text is discarded")
            return
        }

        // Log where the text is going. "Nothing appeared" almost always means
        // it went somewhere unexpected — whichever app had focus when the
        // hotkey fired, which is not necessarily the one being looked at.
        let isASCII = text.allSatisfy(\.isASCII)
        let target = NSWorkspace.shared.frontmostApplication?.localizedName ?? "unknown"
        let method = isASCII ? "keystroke" : "paste"
        Log.ui.notice(
            "injecting \(text.count) chars via \(method, privacy: .public) into \(target, privacy: .public)")
        isASCII ? keystroke(text) : paste(text)
    }

    // MARK: - Keystroke path

    private static func keystroke(_ text: String) {
        guard let source = CGEventSource(stateID: .hidSystemState) else { return }

        for character in text {
            guard let keyCode = Keycodes.forCharacter(character) else {
                // Unmappable ASCII — fall back to pasting the whole string
                // rather than silently dropping a character.
                paste(text)
                return
            }
            let needsShift = character.isUppercase || Keycodes.shiftedCharacters.contains(character)

            for isDown in [true, false] {
                guard let event = CGEvent(
                    keyboardEventSource: source, virtualKey: keyCode, keyDown: isDown
                ) else { continue }
                if needsShift { event.flags = .maskShift }
                event.post(tap: .cghidEventTap)
            }
            // Some apps drop events that arrive too fast.
            usleep(2_000)
        }
    }

    // MARK: - Paste path

    private static func paste(_ text: String) {
        let pasteboard = NSPasteboard.general
        // TODO(phase 2): save and restore the user's clipboard. The Python
        // version left it clobbered too, but it is a real papercut.
        pasteboard.clearContents()
        let wrote = pasteboard.setString(text, forType: .string)
        // Separates "the clipboard write failed" from "the paste keystroke did
        // not land" — otherwise both look identical from the outside.
        Log.ui.notice("pasteboard write: \(wrote, privacy: .public)")

        // Let the clipboard write settle before the paste reads it.
        usleep(10_000)

        guard let source = CGEventSource(stateID: .hidSystemState) else { return }
        let vKey = Keycodes.v

        guard
            let commandDown = CGEvent(keyboardEventSource: source, virtualKey: Keycodes.command, keyDown: true),
            let vDown = CGEvent(keyboardEventSource: source, virtualKey: vKey, keyDown: true),
            let vUp = CGEvent(keyboardEventSource: source, virtualKey: vKey, keyDown: false),
            let commandUp = CGEvent(keyboardEventSource: source, virtualKey: Keycodes.command, keyDown: false)
        else { return }

        vDown.flags = .maskCommand
        vUp.flags = .maskCommand

        for event in [commandDown, vDown, vUp, commandUp] {
            event.post(tap: .cghidEventTap)
        }
        Log.ui.notice("posted paste keystroke")

        usleep(50_000)
    }
}

/// US QWERTY virtual keycodes, carried over from `macos_inject.py`.
///
/// CGEvent posts virtual keycodes that the active layout interprets, so a
/// non-US layout will produce different characters. That is acceptable for the
/// same reason it was in Python: the keystroke path only ever handles ASCII,
/// and the language this app is built for takes the paste path regardless.
enum Keycodes {

    static let command: CGKeyCode = 0x37
    static let v: CGKeyCode = 9

    static let shiftedCharacters: Set<Character> = Set("!@#$%^&*()_+{}|:\"<>?~")

    private static let map: [Character: CGKeyCode] = [
        "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7,
        "c": 8, "v": 9, "b": 11, "q": 12, "w": 13, "e": 14, "r": 15,
        "y": 16, "t": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22,
        "5": 23, "=": 24, "9": 25, "7": 26, "-": 27, "8": 28, "0": 29,
        "]": 30, "o": 31, "u": 32, "[": 33, "i": 34, "p": 35, "l": 37,
        "j": 38, "'": 39, "k": 40, ";": 41, "\\": 42, ",": 43, "/": 44,
        "n": 45, "m": 46, ".": 47, "`": 50, " ": 49,
        "\n": 36,  // return
        "\t": 48,  // tab
    ]

    static func forCharacter(_ character: Character) -> CGKeyCode? {
        map[character] ?? map[Character(character.lowercased())]
    }
}
