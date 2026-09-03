import Foundation

/// A minimal reader/writer for whiz's flat TOML config.
///
/// whiz's config is deliberately simple — a single table of scalar values and
/// string arrays, no nesting, no dates, no inline tables. The Python side
/// hand-rolls its writer for the same reason (`whiz/config.py:_emit_toml`), so
/// this mirrors it rather than pulling in a full TOML dependency.
///
/// Round-trip compatibility with the Python writer is the contract: whatever
/// `whiz config set` produces, this must read, and whatever this writes,
/// `tomllib` must read. `FlatTOMLTests` pins that both ways.
enum FlatTOML {

    enum Value: Equatable {
        case string(String)
        case int(Int)
        case double(Double)
        case bool(Bool)
        case stringArray([String])
        case numberArray([Double])
    }

    // MARK: - Parsing

    /// Parse a flat TOML document. Unparseable lines are skipped rather than
    /// throwing: a config with one bad line should still yield its good keys,
    /// matching how the Python side drops unknown keys instead of failing.
    static func parse(_ text: String) -> [String: Value] {
        var out: [String: Value] = [:]
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespaces) }
        var index = 0
        while index < lines.count {
            let line = lines[index]
            index += 1
            if line.isEmpty || line.hasPrefix("#") || line.hasPrefix("[") { continue }
            guard let eq = line.firstIndex(of: "=") else { continue }
            let key = String(line[line.startIndex..<eq]).trimmingCharacters(in: .whitespaces)
            var rhs = String(line[line.index(after: eq)...]).trimmingCharacters(in: .whitespaces)
            if key.isEmpty { continue }
            // Strip a trailing comment before parsing. Without this, `x = 1 # note`
            // reaches parseValue as "1 # note", fails every branch, and the key
            // vanishes — then `WhizConfig.save()` re-emits only what parsed and
            // deletes it from disk. String and array values happened to survive
            // (their parsers stop at the closing quote/bracket), so the loss hit
            // numbers and bools only, which made it easy to miss.
            if let hash = firstUnquotedHash(in: rhs) {
                rhs = String(rhs[..<hash]).trimmingCharacters(in: .whitespaces)
            }
            // An array value that opens but doesn't close on this line
            // continues over the following lines until its `]` — standard
            // TOML that the shared tuning contract (tuning/tuning.toml) uses
            // for the hallucination phrase list. Comment lines inside the
            // continuation are skipped like comments anywhere else. If the
            // bracket never closes, the key is dropped and scanning resumes
            // at the next line — a malformed value must not swallow the rest
            // of the document.
            if rhs.hasPrefix("["), rhs.firstIndex(of: "]") == nil {
                var joined = rhs
                var lookahead = index
                var closed = false
                var scanned = 0
                while lookahead < lines.count, scanned < 1000 {
                    var next = lines[lookahead]
                    scanned += 1
                    lookahead += 1
                    if let hash = firstUnquotedHash(in: next) {
                        next = String(next[..<hash]).trimmingCharacters(in: .whitespaces)
                    }
                    if next.isEmpty { continue }
                    joined += " " + next
                    if next.contains("]") { closed = true; break }
                }
                guard closed else { continue }
                rhs = joined
                index = lookahead
            }
            if let value = parseValue(rhs) { out[key] = value }
        }
        return out
    }

    /// Index of the first `#` that starts a comment, or nil.
    ///
    /// Quote-aware: `dictate_prompt = "say #1 loudly"` has no comment, and
    /// neither does `model_dirs = ["/a#b"]`. Backslash escapes are honoured so a
    /// `\"` inside a string does not end it prematurely.
    private static func firstUnquotedHash(in s: String) -> String.Index? {
        var inString = false
        var escaped = false
        var index = s.startIndex
        while index < s.endIndex {
            let c = s[index]
            if escaped {
                escaped = false
            } else if c == "\\" && inString {
                escaped = true
            } else if c == "\"" {
                inString.toggle()
            } else if c == "#" && !inString {
                return index
            }
            index = s.index(after: index)
        }
        return nil
    }

    private static func parseValue(_ raw: String) -> Value? {
        if raw.hasPrefix("\"") { return unquote(raw).map { .string($0) } }
        if raw == "true" { return .bool(true) }
        if raw == "false" { return .bool(false) }
        if raw.hasPrefix("[") { return parseArray(raw) }
        if let i = Int(raw) { return .int(i) }
        if let d = Double(raw) { return .double(d) }
        return nil
    }

    /// Strip surrounding quotes and unescape `\\` and `\"` — the only two
    /// escapes the Python writer emits.
    private static func unquote(_ raw: String) -> String? {
        guard raw.count >= 2, raw.hasPrefix("\"") else { return nil }
        // Find the closing quote, honouring backslash escapes.
        let chars = Array(raw.dropFirst())
        var out = ""
        var i = 0
        while i < chars.count {
            let c = chars[i]
            if c == "\\", i + 1 < chars.count {
                let next = chars[i + 1]
                if next == "\\" || next == "\"" {
                    out.append(next)
                    i += 2
                    continue
                }
            }
            if c == "\"" { return out }
            out.append(c)
            i += 1
        }
        return nil  // unterminated string
    }

    private static func parseArray(_ raw: String) -> Value? {
        guard raw.hasPrefix("["), let close = raw.lastIndex(of: "]") else { return nil }
        let inner = String(raw[raw.index(after: raw.startIndex)..<close])
            .trimmingCharacters(in: .whitespaces)
        if inner.isEmpty { return .stringArray([]) }

        let parts = splitTopLevel(inner)
        var strings: [String] = []
        var numbers: [Double] = []
        for part in parts {
            let p = part.trimmingCharacters(in: .whitespaces)
            if p.hasPrefix("\"") {
                guard let s = unquote(p) else { return nil }
                strings.append(s)
            } else if let d = Double(p) {
                numbers.append(d)
            } else {
                return nil
            }
        }
        // Mixed arrays aren't emitted by the Python side; prefer strings.
        if !strings.isEmpty && numbers.isEmpty { return .stringArray(strings) }
        if strings.isEmpty && !numbers.isEmpty { return .numberArray(numbers) }
        return nil
    }

    /// Split on commas that are not inside a quoted string.
    private static func splitTopLevel(_ s: String) -> [String] {
        var out: [String] = []
        var current = ""
        var inString = false
        var escaped = false
        for c in s {
            if escaped { current.append(c); escaped = false; continue }
            if c == "\\" && inString { current.append(c); escaped = true; continue }
            if c == "\"" { inString.toggle(); current.append(c); continue }
            if c == "," && !inString { out.append(current); current = ""; continue }
            current.append(c)
        }
        if !current.trimmingCharacters(in: .whitespaces).isEmpty { out.append(current) }
        return out
    }

    // MARK: - Emitting

    /// Emit one `key = value` line per entry, sorted for stable diffs.
    static func emit(_ values: [String: Value]) -> String {
        var lines: [String] = []
        for key in values.keys.sorted() {
            lines.append("\(key) = \(literal(values[key]!))")
        }
        return lines.joined(separator: "\n") + "\n"
    }

    private static func literal(_ value: Value) -> String {
        switch value {
        case .string(let s): return quote(s)
        case .int(let i): return String(i)
        case .double(let d): return String(d)
        case .bool(let b): return b ? "true" : "false"
        case .stringArray(let items):
            return "[" + items.map(quote).joined(separator: ", ") + "]"
        case .numberArray(let items):
            return "[" + items.map { String($0) }.joined(separator: ", ") + "]"
        }
    }

    private static func quote(_ s: String) -> String {
        let escaped = s
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return "\"\(escaped)\""
    }
}
