// A minimal JSON reader/WRITER pair, shared by the golden-corpus tests and
// the Linux control socket. Reads expected.json and the socket's incoming
// commands; writes the socket's outgoing events and replies.
//
// Deliberately small: objects, arrays, strings, numbers, booleans, null.
// No floats-vs-ints distinction (numbers are doubles), no escapes beyond
// " \\ \/ \b \f \n \r \t \uXXXX (which the corpus does not use, but a
// regenerated file might).
//
// The namespace is mynah::json; the tests use it by the same spelling.

#pragma once

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mynah::json {

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct Value {
    enum class Tag { Null, Bool, Number, String, Array, Object };

    Tag tag = Tag::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Value> array;
    Object object;

    // Declared here, defaulted below the class: implicit ones are defined
    // where Value is still incomplete for std::pair<std::string, Value>, which
    // Clang with libstdc++ 16 rejects (GCC and libc++ happen to accept it).
    Value();
    Value(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(const Value&);
    Value& operator=(Value&&) noexcept;
    ~Value();

    bool is_object() const { return tag == Tag::Object; }
    bool is_array() const { return tag == Tag::Array; }
    bool is_number() const { return tag == Tag::Number; }
    bool is_bool() const { return tag == Tag::Bool; }

    // Object member lookup; nullptr when absent.
    const Value* find(std::string_view key) const {
        if (tag != Tag::Object) return nullptr;
        for (const auto& [name, value] : object)
            if (name == key) return &value;
        return nullptr;
    }

    // Array element lookup; nullptr when out of range.
    const Value* at(std::size_t index) const {
        if (tag != Tag::Array || index >= array.size()) return nullptr;
        return &array[index];
    }
};

inline Value::Value() = default;
inline Value::Value(const Value&) = default;
inline Value::Value(Value&&) noexcept = default;
inline Value& Value::operator=(const Value&) = default;
inline Value& Value::operator=(Value&&) noexcept = default;
inline Value::~Value() = default;

namespace detail {

inline void skip_ws(std::string_view text, std::size_t& pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
        ++pos;
}

inline std::string parse_string(std::string_view text, std::size_t& pos) {
    if (pos >= text.size() || text[pos] != '"')
        throw std::runtime_error("json: string must start with a quote");
    ++pos;
    std::string out;
    while (pos < text.size() && text[pos] != '"') {
        char c = text[pos++];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (pos >= text.size()) throw std::runtime_error("json: truncated escape");
        char e = text[pos++];
        switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            if (pos + 4 > text.size()) throw std::runtime_error("json: truncated \\u");
            std::uint32_t cp = 0;
            for (int i = 0; i < 4; ++i) {
                char h = text[pos++];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= h - '0';
                else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                else throw std::runtime_error("json: bad hex digit");
            }
            // Encode as UTF-8; surrogate pairs are not needed by the corpus
            // but a regenerated file is still within reach.
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
            } else {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: throw std::runtime_error("json: unknown escape");
        }
    }
    if (pos >= text.size()) throw std::runtime_error("json: unterminated string");
    ++pos; // closing quote
    return out;
}

inline Value parse_value(std::string_view text, std::size_t& pos);

inline Value parse_object(std::string_view text, std::size_t& pos) {
    Value value;
    value.tag = Value::Tag::Object;
    ++pos; // {
    skip_ws(text, pos);
    if (pos < text.size() && text[pos] == '}') {
        ++pos;
        return value;
    }
    while (true) {
        skip_ws(text, pos);
        std::string key = parse_string(text, pos);
        skip_ws(text, pos);
        if (pos >= text.size() || text[pos] != ':')
            throw std::runtime_error("json: expected ':'");
        ++pos;
        value.object.emplace_back(std::move(key), parse_value(text, pos));
        skip_ws(text, pos);
        if (pos >= text.size()) throw std::runtime_error("json: truncated object");
        if (text[pos] == ',') {
            ++pos;
            continue;
        }
        if (text[pos] == '}') {
            ++pos;
            return value;
        }
        throw std::runtime_error("json: expected ',' or '}'");
    }
}

inline Value parse_array(std::string_view text, std::size_t& pos) {
    Value value;
    value.tag = Value::Tag::Array;
    ++pos; // [
    skip_ws(text, pos);
    if (pos < text.size() && text[pos] == ']') {
        ++pos;
        return value;
    }
    while (true) {
        value.array.push_back(parse_value(text, pos));
        skip_ws(text, pos);
        if (pos >= text.size()) throw std::runtime_error("json: truncated array");
        if (text[pos] == ',') {
            ++pos;
            continue;
        }
        if (text[pos] == ']') {
            ++pos;
            return value;
        }
        throw std::runtime_error("json: expected ',' or ']'");
    }
}

inline Value parse_value(std::string_view text, std::size_t& pos) {
    skip_ws(text, pos);
    if (pos >= text.size()) throw std::runtime_error("json: empty");
    char c = text[pos];
    if (c == '{') return parse_object(text, pos);
    if (c == '[') return parse_array(text, pos);
    if (c == '"') {
        Value value;
        value.tag = Value::Tag::String;
        value.text = parse_string(text, pos);
        return value;
    }
    if (text.compare(pos, 4, "true") == 0) {
        pos += 4;
        Value value;
        value.tag = Value::Tag::Bool;
        value.boolean = true;
        return value;
    }
    if (text.compare(pos, 5, "false") == 0) {
        pos += 5;
        Value value;
        value.tag = Value::Tag::Bool;
        value.boolean = false;
        return value;
    }
    if (text.compare(pos, 4, "null") == 0) {
        pos += 4;
        Value value;
        return value;
    }
    // Number: [-]digits[.digits][e[+-]digits]. Apple's libc++ has no
    // floating-point from_chars at the 13.0 deployment target, so strtod
    // it is — a test binary never calls setlocale, so the C locale holds.
    std::size_t begin = pos;
    if (c == '-' || c == '+') ++pos;
    while (pos < text.size() &&
           ((text[pos] >= '0' && text[pos] <= '9') || text[pos] == '.' ||
            text[pos] == 'e' || text[pos] == 'E' || text[pos] == '+' || text[pos] == '-'))
        ++pos;
    std::string number_text(text.substr(begin, pos - begin));
    char* end = nullptr;
    double number = std::strtod(number_text.c_str(), &end);
    if (end != number_text.c_str() + number_text.size())
        throw std::runtime_error("json: bad number");
    Value value;
    value.tag = Value::Tag::Number;
    value.number = number;
    return value;
}

} // namespace

inline Value parse(std::string_view text) {
    std::size_t pos = 0;
    Value value = detail::parse_value(text, pos);
    detail::skip_ws(text, pos);
    if (pos != text.size()) throw std::runtime_error("json: trailing content");
    return value;
}

// --- writing -----------------------------------------------------------------

// Escape one string for embedding in JSON output.
inline std::string escape(std::string_view text) {
    std::string out;
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    return out;
}

inline std::string quoted(std::string_view text) { return "\"" + escape(text) + "\""; }

} // namespace mynah::json