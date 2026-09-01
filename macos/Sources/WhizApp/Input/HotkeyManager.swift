import AppKit
import Carbon.HIToolbox

/// Global hotkey registration via Carbon's `RegisterEventHotKey`.
///
/// Replaces the `pynput` listener. Two reasons this is better than the
/// alternative (`NSEvent.addGlobalMonitorForEvents`): the hotkey is *consumed*
/// rather than merely observed, so it does not leak through to the focused app,
/// and registration does not itself require Accessibility. Text injection still
/// does — but the hotkey arming and the injection permission are now separate
/// concerns, so the app can show a live "press the hotkey" state during
/// onboarding before the user has granted anything.
@MainActor
final class HotkeyManager {

    private var hotKeyRef: EventHotKeyRef?
    private var eventHandler: EventHandlerRef?
    private var onTrigger: (() -> Void)?

    private static let signature: OSType = 0x77_68_7A_6B  // 'whzk'

    /// Register `spec` (pynput syntax, e.g. `<cmd>+<shift>+.`). Returns false
    /// if the spec cannot be parsed or the combination is already claimed by
    /// another app.
    @discardableResult
    func register(_ spec: String, onTrigger: @escaping () -> Void) -> Bool {
        unregister()

        guard let combo = HotkeySpec.parse(spec) else {
            NSLog("whiz: could not parse hotkey '\(spec)'")
            return false
        }
        self.onTrigger = onTrigger

        var eventType = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed)
        )
        let context = Unmanaged.passUnretained(self).toOpaque()

        InstallEventHandler(GetApplicationEventTarget(), hotkeyHandler, 1, &eventType, context, &eventHandler)

        let id = EventHotKeyID(signature: Self.signature, id: 1)
        let status = RegisterEventHotKey(
            combo.keyCode, combo.modifiers, id, GetApplicationEventTarget(), 0, &hotKeyRef
        )
        if status != noErr {
            NSLog("whiz: hotkey '\(spec)' is unavailable (status \(status)) — likely claimed by another app")
            return false
        }
        return true
    }

    func unregister() {
        if let hotKeyRef { UnregisterEventHotKey(hotKeyRef) }
        if let eventHandler { RemoveEventHandler(eventHandler) }
        hotKeyRef = nil
        eventHandler = nil
        onTrigger = nil
    }

    fileprivate func fire() {
        onTrigger?()
    }
}

/// Carbon calls this from the main run loop.
private let hotkeyHandler: EventHandlerUPP = { _, _, context in
    guard let context else { return noErr }
    let manager = Unmanaged<HotkeyManager>.fromOpaque(context).takeUnretainedValue()
    MainActor.assumeIsolated { manager.fire() }
    return noErr
}

/// Parses pynput-style hotkey strings so the config file stays readable by both
/// implementations. Changing the syntax would mean every existing user's
/// `dictate_hotkey` silently stopped working.
enum HotkeySpec {

    struct Combo {
        var keyCode: UInt32
        var modifiers: UInt32
    }

    static func parse(_ spec: String) -> Combo? {
        let tokens = spec.split(separator: "+").map {
            $0.trimmingCharacters(in: .whitespaces).lowercased()
        }
        guard !tokens.isEmpty else { return nil }

        var modifiers: UInt32 = 0
        var keyToken: String?

        for token in tokens {
            let bare = token.hasPrefix("<") && token.hasSuffix(">")
                ? String(token.dropFirst().dropLast())
                : token
            switch bare {
            case "cmd", "command":  modifiers |= UInt32(cmdKey)
            case "shift":           modifiers |= UInt32(shiftKey)
            case "ctrl", "control": modifiers |= UInt32(controlKey)
            case "alt", "option":   modifiers |= UInt32(optionKey)
            default:                keyToken = bare
            }
        }

        guard let keyToken, let keyCode = keyCode(for: keyToken) else { return nil }
        return Combo(keyCode: keyCode, modifiers: modifiers)
    }

    private static func keyCode(for token: String) -> UInt32? {
        if let function = functionKeys[token] { return function }
        if token.count == 1, let character = token.first,
           let code = Keycodes.forCharacter(character) {
            return UInt32(code)
        }
        return namedKeys[token]
    }

    private static let functionKeys: [String: UInt32] = [
        "f1": 122, "f2": 120, "f3": 99, "f4": 118, "f5": 96, "f6": 97,
        "f7": 98, "f8": 100, "f9": 101, "f10": 109, "f11": 103, "f12": 111,
    ]

    private static let namedKeys: [String: UInt32] = [
        "space": 49, "return": 36, "enter": 36, "tab": 48, "escape": 53, "esc": 53,
        "period": 47, "comma": 43, "slash": 44, "backslash": 42,
    ]
}
