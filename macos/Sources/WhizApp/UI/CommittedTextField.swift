import SwiftUI

/// A text field that writes its value on Return or when focus leaves, not on
/// every keystroke — and refuses to commit something invalid.
///
/// The settings fields were bound straight through to `WhizConfig`, whose setter
/// saves to disk. So editing `ru` into `en` rewrote the config file four times
/// and briefly persisted `r` and `""`, either of which would have been used had
/// a dictation session started mid-edit. For the multi-line prompt it was a
/// write per character.
///
/// Validation is the other half: `HotkeySpec.parse` already rejects a malformed
/// hotkey, but nothing surfaced that — the field accepted it, the config stored
/// it, and registration failed later in a log line nobody reads.
struct CommittedTextField: View {

    var placeholder: String = ""
    @Binding var value: String

    /// Return an error message to reject the edit, or nil to accept it.
    var validate: ((String) -> String?)?

    var width: CGFloat?

    @State private var draft: String = ""
    @State private var error: String?
    @FocusState private var isFocused: Bool

    var body: some View {
        VStack(alignment: .trailing, spacing: 4) {
            TextField(placeholder, text: $draft)
                .focused($isFocused)
                .frame(width: width)
                .onSubmit { commit() }
                // Single-parameter onChange: the two-parameter form is macOS 14,
                // and the deployment target is 13.
                .onChange(of: isFocused) { focused in
                    if !focused { commit() }
                }
                .onAppear { draft = value }
                .onChange(of: value) { newValue in
                    // Keep in step when the value changes elsewhere (config
                    // reload, Restore Defaults), but never while being edited.
                    if !isFocused { draft = newValue }
                }
            if let error {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        }
    }

    private func commit() {
        let trimmed = draft.trimmingCharacters(in: .whitespaces)
        if let validate, let message = validate(trimmed) {
            error = message
            return
        }
        error = nil
        draft = trimmed
        if trimmed != value { value = trimmed }
    }
}

/// A multi-line editor that commits on focus loss.
///
/// `TextEditor` has no `onSubmit` — Return inserts a newline, which the Russian
/// prompt legitimately contains — so focus loss is the only sensible commit
/// point. Without this, every character of a long prompt rewrote the config.
struct PromptEditor: View {
    @Binding var text: String

    @State private var draft: String = ""
    @FocusState private var isFocused: Bool

    var body: some View {
        TextEditor(text: $draft)
            .focused($isFocused)
            .font(.system(.body, design: .monospaced))
            .frame(height: 60)
            .onAppear { draft = text }
            .onChange(of: isFocused) { focused in
                if !focused, draft != text { text = draft }
            }
            .onChange(of: text) { newValue in
                if !isFocused { draft = newValue }
            }
    }
}
