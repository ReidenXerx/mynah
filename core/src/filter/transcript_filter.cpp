#include "transcript_filter.hpp"

#include <string>

namespace mynah::filter {

namespace {

constexpr bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Lowercase one UTF-8 code point in place; advances i past it. Handles the
// scripts the phrase list and its tests use: ASCII, Latin-1 supplement
// (À..Þ except ×), and Cyrillic (А..Я, Ё). See the header caveat.
void lower_codepoint(std::string& s, std::size_t& i) {
    unsigned char lead = static_cast<unsigned char>(s[i]);
    // ASCII
    if (lead < 0x80) {
        if (lead >= 'A' && lead <= 'Z') s[i] = static_cast<char>(lead + 0x20);
        ++i;
        return;
    }
    // Two-byte sequences: U+0080..U+07FF — Latin-1 supplement and Cyrillic.
    if (lead >= 0xC0 && lead < 0xE0 && i + 1 < s.size()) {
        unsigned char second = static_cast<unsigned char>(s[i + 1]);
        if ((second & 0xC0) == 0x80) {
            unsigned int cp = (static_cast<unsigned int>(lead & 0x1F) << 6) |
                              (second & 0x3F);
            unsigned int lowered = cp;
            if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) || (cp >= 0x410 && cp <= 0x42F))
                lowered = cp + 0x20; // À..Þ, А..Я
            else if (cp == 0x401)
                lowered = 0x451; // Ё -> ё
            if (lowered != cp) {
                s[i] = static_cast<char>(0xC0 | (lowered >> 6));
                s[i + 1] = static_cast<char>(0x80 | (lowered & 0x3F));
            }
            i += 2;
            return;
        }
    }
    // Anything else passes through: three- and four-byte sequences, lone
    // continuation bytes (invalid UTF-8 from a decoder glitch).
    if (lead >= 0xF0 && i + 3 < s.size()) i += 4;
    else if (lead >= 0xE0 && i + 2 < s.size()) i += 3;
    else if (lead >= 0x80) i += 1;
    else i += 1;
}

// The trimmed, lowercased transcript both match modes work on.
std::string normalize(std::string_view text) {
    std::string s(text);
    for (std::size_t i = 0; i < s.size();) lower_codepoint(s, i);
    // Trim after lowercasing: the ASCII spaces and control chars that split
    // lines are stable under both operations.
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && is_ascii_space(s[begin])) ++begin;
    while (end > begin && is_ascii_space(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

} // namespace

bool is_hallucination(std::string_view text) {
    std::string normalized = normalize(text);
    if (normalized.empty()) return true;
    // Artifacts match by substring anywhere; vocab words match only as the
    // whole utterance.
    for (std::string_view phrase : artifact_phrases)
        if (normalized.find(phrase) != std::string::npos) return true;
    for (std::string_view phrase : vocab_phrases)
        if (normalized == phrase) return true;
    return false;
}

} // namespace mynah::filter