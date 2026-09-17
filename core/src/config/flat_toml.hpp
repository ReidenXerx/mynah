// mynah::flat_toml — a minimal reader/writer for mynah's flat TOML config.
//
// mynah's config is deliberately simple — a single table of scalars and
// string/number arrays, no nesting, no dates, no inline tables. Python
// hand-rolls its writer for the same reason (mynah/config.py:_emit_toml) and
// the Swift app reads and writes the exact same shape (FlatTOML.swift). This
// is the C++ mirror: the fourth implementation of one small format, so that
// all four can share one file on disk.
//
// Round-trip compatibility is the contract, pinned by core/tests/test_toml.cpp
// (ported from FlatTOMLTests in macos/Tests/MynahAppTests/ConfigTests.swift):
// whatever any writer produces, every reader must read; whatever a reader
// preserves, its writer must keep byte-stable on save.
//
// Parser semantics, inherited from FlatTOML.swift rather than from TOML:
//   - Unparseable lines are skipped, not fatal — a config with one bad line
//     still yields its good keys, and a save re-emits only what parsed.
//     (Python's tomllib is stricter and raises on the whole file; that
//     difference is a documented divergence, recorded in the Swift port.)
//   - Table headers ([section]) are skipped: flat keys only.
//   - Comments after a value are stripped, quote-aware.
//   - Arrays may span lines (the shared tuning contract uses this for its
//     phrase list); a never-closed array drops only its own key.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mynah::flat_toml {

// NOTE: constructing a Value from a literal needs care — `Value("ru")` would
// resolve const char* to the bool alternative. Use the maker functions or
// spell the type out.
using Value = std::variant<std::string, int64_t, double, bool,
                            std::vector<std::string>, std::vector<double>>;

// Key-sorted, so emit() output is stable for diffs.
using Table = std::map<std::string, Value>;

inline Value str(std::string s) { return Value(std::move(s)); }
inline Value integer(std::int64_t i) { return Value(i); }
inline Value real(double d) { return Value(d); }
inline Value boolean(bool b) { return Value(b); }
inline Value strings(std::vector<std::string> v) { return Value(std::move(v)); }
inline Value numbers(std::vector<double> v) { return Value(std::move(v)); }

// Parse a flat TOML document. Never throws; unknown/broken lines are skipped.
Table parse(std::string_view text);

// One `key = value` line per entry, keys sorted, trailing newline — the same
// shape Python's _emit_toml and Swift's FlatTOML.emit produce.
std::string emit(const Table& values);

} // namespace mynah::flat_toml