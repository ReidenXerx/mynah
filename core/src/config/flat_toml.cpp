#include "flat_toml.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace mynah::flat_toml {

namespace {

// strtod and snprintf follow the process locale, and a front end may set one:
// QCoreApplication calls setlocale(LC_ALL, "") on Linux, so under uk_UA or
// ru_RU the decimal separator becomes ',' — reading "0.01" stops at the dot
// and drops the key, and writing emits "0,02", which no reader accepts. Python
// and Swift parse numbers locale-independently; this makes the core do the
// same. uselocale() switches only the calling thread, so the app's own locale
// is untouched everywhere else.
class CLocaleScope {
public:
    CLocaleScope() : previous_(uselocale(c_locale())) {}
    ~CLocaleScope() { uselocale(previous_); }
    CLocaleScope(const CLocaleScope&) = delete;
    CLocaleScope& operator=(const CLocaleScope&) = delete;

private:
    static locale_t c_locale() {
        // Never freed: one handle for the life of the process. If newlocale
        // fails it returns 0, and uselocale(0) changes nothing.
        static const locale_t locale = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(0));
        return locale;
    }
    locale_t previous_;
};

constexpr bool is_space(char c) { return c == ' ' || c == '\t'; }

// Trim ASCII whitespace. Lines are already free of \r/\n (split_lines), and
// the config's values are UTF-8 where a stray exotic space is not something
// any writer has ever emitted.
std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// Split honouring all three line terminators: \r\n as one, then lone \r and
// lone \n. CRLF-saved configs (M7 in the Swift audit) arrive clean.
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') {
            lines.push_back(text.substr(start, i - start));
            start = ++i;
        } else if (text[i] == '\r') {
            lines.push_back(text.substr(start, i - start));
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            start = ++i;
        } else {
            ++i;
        }
    }
    lines.push_back(text.substr(start));
    return lines;
}

// Index of the first `#` that starts a comment — quote-aware, honouring
// backslash escapes so a `\"` inside a string does not end it. Without this,
// `prompt = "say #1 loudly"` loses its value, and `model_dirs = ["/a#b"]` its
// key (see ConfigTests.swift: "does not mistake a # inside a value").
std::size_t first_unquoted_hash(std::string_view s) {
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (escaped) {
            escaped = false;
        } else if (in_string && c == '\\') {
            escaped = true;
        } else if (c == '"') {
            in_string = !in_string;
        } else if (c == '#' && !in_string) {
            return i;
        }
    }
    return std::string_view::npos;
}

bool contains_close_bracket(std::string_view s) {
    return s.find(']') != std::string_view::npos;
}

// Strip surrounding quotes and unescape — symmetric with every writer's
// escaping (backslash, quote, then \n/\r/\t). A control char that reaches the
// file as a raw byte splits the line and loses the key (C1, wave-1 audit).
// Returns false for an unterminated string. Anything after the closing quote
// is ignored, matching the Swift parser.
bool unquote(std::string_view raw, std::string& out) {
    if (raw.size() < 2 || raw.front() != '"') return false;
    out.clear();
    std::size_t i = 1;
    while (i < raw.size()) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            switch (next) {
            case '\\':
            case '"': out.push_back(next); i += 2; continue;
            case 'n': out.push_back('\n'); i += 2; continue;
            case 'r': out.push_back('\r'); i += 2; continue;
            case 't': out.push_back('\t'); i += 2; continue;
            default: break; // not one of ours — keep the backslash
            }
        }
        if (c == '"') return true; // closing quote
        out.push_back(c);
        ++i;
    }
    return false; // unterminated
}

// Split on commas that are not inside a quoted string, keeping escapes intact
// so `\"` survives to unquote().
std::vector<std::string_view> split_top_level(std::string_view s) {
    std::vector<std::string_view> out;
    bool in_string = false;
    bool escaped = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (escaped) { escaped = false; continue; }
        if (in_string && c == '\\') { escaped = true; continue; }
        if (c == '"') { in_string = !in_string; continue; }
        if (c == ',' && !in_string) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    std::string_view tail = s.substr(start);
    if (!trim(tail).empty()) out.push_back(tail);
    return out;
}

// Swift's Int(String): base-10, optional sign, digits only, no underscores,
// no trimming (the caller trims). Rejects overflow, like Int does.
bool parse_int(std::string_view raw, std::int64_t& out) {
    if (raw.empty()) return false;
    bool negative = raw[0] == '-';
    // Swift's Int accepts a leading '+'; from_chars does not, so skip it.
    if (raw[0] == '+' || raw[0] == '-') raw.remove_prefix(1);
    if (raw.empty()) return false;
    for (char c : raw)
        if (c < '0' || c > '9') return false;
    auto begin = raw.data();
    auto end = raw.data() + raw.size();
    if (negative) {
        // Negate the unsigned parse so INT64_MIN parses exactly.
        std::uint64_t magnitude = 0;
        auto [ptr, ec] = std::from_chars(begin, end, magnitude);
        if (ec != std::errc{} || ptr != end || magnitude > 9223372036854775808ULL)
            return false;
        out = magnitude == 9223372036854775808ULL
                  ? std::numeric_limits<std::int64_t>::min()
                  : -static_cast<std::int64_t>(magnitude);
        return true;
    }
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

// strtod with Swift's Double(String) behaviour for everything the tests pin:
// full consumption, no trailing garbage, "inf"/"nan" accepted like Double.
bool parse_double(std::string_view raw, double& out) {
    if (raw.empty()) return false;
    // Null-terminate for the C API without copying twice.
    std::string buf(raw);
    char* end = nullptr;
    CLocaleScope c_locale;
    out = std::strtod(buf.c_str(), &end);
    return end == buf.c_str() + buf.size();
}

Value* parse_array_into(std::string_view raw, Value& out) {
    if (raw.empty() || raw.front() != '[') return nullptr;
    std::size_t close = raw.rfind(']');
    if (close == std::string_view::npos) return nullptr;
    std::string_view inner = trim(raw.substr(1, close - 1));
    if (inner.empty()) {
        out = strings({});
        return &out;
    }
    std::vector<std::string> items;
    std::vector<double> numbers;
    for (std::string_view part : split_top_level(inner)) {
        std::string_view p = trim(part);
        if (!p.empty() && p.front() == '"') {
            std::string s;
            if (!unquote(p, s)) return nullptr;
            items.push_back(std::move(s));
        } else {
            double d = 0.0;
            if (!parse_double(p, d)) return nullptr;
            numbers.push_back(d);
        }
    }
    // Mixed arrays are emitted by no writer; refuse them like the others do.
    if (!items.empty() && numbers.empty()) out = strings(std::move(items));
    else if (items.empty() && !numbers.empty()) out = flat_toml::numbers(std::move(numbers));
    else return nullptr;
    return &out;
}

bool parse_value(std::string_view raw, Value& out) {
    if (raw.empty()) return false;
    if (raw.front() == '"') {
        std::string s;
        if (!unquote(raw, s)) return false;
        out = str(std::move(s));
        return true;
    }
    if (raw == "true") { out = boolean(true); return true; }
    if (raw == "false") { out = boolean(false); return true; }
    if (raw.front() == '[') return parse_array_into(raw, out) != nullptr;
    std::int64_t i = 0;
    if (parse_int(raw, i)) { out = integer(i); return true; }
    double d = 0.0;
    if (parse_double(raw, d)) { out = real(d); return true; }
    return false;
}

// Python repr() for doubles: the shortest string that round-trips, always
// with a decimal point or exponent so the value stays a float on re-read
// (repr(45.0) == "45.0"). Apple's libc++ has no floating-point to_chars, so
// this is the portable way: the fewest significant digits that parse back
// exactly, then laid out the way repr lays them out — positional for
// exponents -4..15 ("10.0", "0.0001"), scientific outside ("1e+16",
// "1e-05"). Plain %g was the old layout, and it goes scientific as soon as
// the exponent reaches the digit count: 10.0 came out as "1e+01", which is
// what `auto_stop_silence = 10` was written back as.
std::string format_double(double d) { return number_to_string(d); }

std::string format_double_impl(double d) {
    CLocaleScope c_locale;
    char buf[64];
    if (!std::isfinite(d)) { // "inf", "-inf", "nan": already floats on re-read
        std::snprintf(buf, sizeof(buf), "%g", d);
        return buf;
    }
    int digits = 17; // 17 significant digits always round-trip
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*e", precision - 1, d);
        if (std::strtod(buf, nullptr) == d) {
            digits = precision;
            break;
        }
    }
    std::snprintf(buf, sizeof(buf), "%.*e", digits - 1, d);
    const int exponent = std::atoi(std::strchr(buf, 'e') + 1);
    if (exponent < -4 || exponent >= 16) return buf; // "1e+16", "1.5e-05"
    std::snprintf(buf, sizeof(buf), "%.*f", std::max(digits - 1 - exponent, 0), d);
    std::string s(buf);
    if (s.find('.') == std::string::npos) s += ".0"; // "45" -> "45.0"
    return s;
}

std::string escape(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    out += '"';
    return out;
}

std::string format_value(const Value& v) {
    return std::visit([](auto&& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return escape(value);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return std::to_string(value);
        } else if constexpr (std::is_same_v<T, double>) {
            return format_double(value);
        } else if constexpr (std::is_same_v<T, bool>) {
            return value ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            std::string out = "[";
            bool first = true;
            for (const auto& item : value) {
                if (!first) out += ", ";
                first = false;
                out += escape(item);
            }
            return out + "]";
        } else {
            std::string out = "[";
            bool first = true;
            for (double item : value) {
                if (!first) out += ", ";
                first = false;
                out += format_double(item);
            }
            return out + "]";
        }
    }, v);
}

} // namespace

std::string number_to_string(double value) { return format_double_impl(value); }

Table parse(std::string_view text) {
    Table out;
    std::vector<std::string_view> lines = split_lines(text);
    std::size_t index = 0;
    while (index < lines.size()) {
        std::string_view line = trim(lines[index]);
        ++index;
        if (line.empty() || line.front() == '#' || line.front() == '[') continue;
        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = trim(line.substr(0, eq));
        std::string_view rhs = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        // Strip a trailing comment before parsing, or `x = 1 # note` fails
        // every branch and the key vanishes — then the next save deletes it
        // from disk.
        std::size_t hash = first_unquoted_hash(rhs);
        if (hash != std::string_view::npos) rhs = trim(rhs.substr(0, hash));

        // An array that opens but does not close on this line continues over
        // the following lines until its `]` (tuning.toml's phrase list).
        // Comment lines inside the continuation are skipped. If the bracket
        // never closes, only this key is dropped and scanning resumes at the
        // next line — a malformed value must not swallow the document.
        bool parsed_ok = false;
        Value value;
        if (!rhs.empty() && rhs.front() == '[' && !contains_close_bracket(rhs)) {
            std::string joined(rhs);
            std::size_t lookahead = index;
            bool closed = false;
            std::size_t scanned = 0;
            while (lookahead < lines.size() && scanned < 1000) {
                std::string_view next = trim(lines[lookahead]);
                ++scanned;
                ++lookahead;
                std::size_t next_hash = first_unquoted_hash(next);
                if (next_hash != std::string_view::npos)
                    next = trim(next.substr(0, next_hash));
                if (next.empty()) continue;
                joined += ' ';
                joined += next;
                if (next.find(']') != std::string_view::npos) {
                    closed = true;
                    break;
                }
            }
            if (closed) {
                // Parse before joined leaves scope: a string_view into it
                // would dangle.
                parsed_ok = parse_value(joined, value);
                index = lookahead;
            }
        } else {
            parsed_ok = parse_value(rhs, value);
        }
        if (parsed_ok) out[std::string(key)] = std::move(value);
    }
    return out;
}

std::string emit(const Table& values) {
    std::string out;
    for (const auto& [key, value] : values) {
        out += key;
        out += " = ";
        out += format_value(value);
        out += '\n';
    }
    return out;
}

} // namespace mynah::flat_toml