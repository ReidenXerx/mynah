// FlatTOML: the config file is co-owned by the core, the Python CLI and the
// Swift app, so these are compatibility tests against both other writers.
// Ported from FlatTOMLTests in macos/Tests/MynahAppTests/ConfigTests.swift;
// the last three cases mirror TuningTests.swift (the shape of
// tuning/tuning.toml's phrase lists). When a case here changes, change its
// twins — or the ports drift, which is the bug class this suite exists to
// catch.

#include "vendor/doctest.h"

#include <clocale>
#include <string>
#include <vector>

#include "config/flat_toml.hpp"

using mynah::flat_toml::Table;
using mynah::flat_toml::Value;

namespace {

// Table lookup that fails loudly on a missing key (std::map's operator[]
// would silently insert a default Value and could pass a comparison).
const Value* get(const Table& values, const std::string& key) {
    auto it = values.find(key);
    if (it == values.end()) return nullptr;
    return &it->second;
}

// Makers: Value from a literal is ambiguous (const char* wants to become a
// bool), so every construction in these tests goes through these.
Value S(const char* s) { return Value(std::string(s)); }
Value I(long long i) { return Value(static_cast<std::int64_t>(i)); }
Value D(double d) { return Value(d); }
Value B(bool b) { return Value(b); }
Value SA(std::vector<std::string> v) { return Value(std::move(v)); }

} // namespace

TEST_CASE("parses the scalar types the Python writer emits") {
    Table values = mynah::flat_toml::parse(R"(
        language = "ru"
        idle_timeout = 45.0
        ai_max_frames = 50
        vad = true
        idle_visible = false
    )");

    REQUIRE(get(values, "language"));
    CHECK(*get(values, "language") == S("ru"));
    REQUIRE(get(values, "idle_timeout"));
    CHECK(*get(values, "idle_timeout") == D(45.0));
    REQUIRE(get(values, "ai_max_frames"));
    CHECK(*get(values, "ai_max_frames") == I(50));
    REQUIRE(get(values, "vad"));
    CHECK(*get(values, "vad") == B(true));
    REQUIRE(get(values, "idle_visible"));
    CHECK(*get(values, "idle_visible") == B(false));
}

TEST_CASE("handles the escapes the Python writer emits") {
    Table values = mynah::flat_toml::parse(R"(prompt = "say \"hi\" and \\ then stop")");
    REQUIRE(get(values, "prompt"));
    CHECK(*get(values, "prompt") == S(R"(say "hi" and \ then stop)"));
}

TEST_CASE("unescapes the control chars the Python writer emits (C1)") {
    // A prompt containing a newline was once written as a raw byte: the line
    // split in two and the key was lost or corrupted on every side. The
    // writer escapes, the parser unescapes, and emit must re-escape.
    Table values = mynah::flat_toml::parse(R"(prompt = "line1\nline2\r\nline3\tend")");
    REQUIRE(get(values, "prompt"));
    CHECK(*get(values, "prompt") == Value(std::string("line1\nline2\r\nline3\tend")));

    Table round = mynah::flat_toml::parse(mynah::flat_toml::emit(
        Table{{"prompt", Value(std::string("a\nb\tc\rd"))}}));
    REQUIRE(get(round, "prompt"));
    CHECK(*get(round, "prompt") == Value(std::string("a\nb\tc\rd")));
}

TEST_CASE("parses a document written with CRLF line endings (M7)") {
    Table values = mynah::flat_toml::parse("language = \"ru\"\r\nvad = true\r\n");
    REQUIRE(get(values, "language"));
    CHECK(*get(values, "language") == S("ru"));
    REQUIRE(get(values, "vad"));
    CHECK(*get(values, "vad") == B(true));
}

TEST_CASE("parses string arrays, including empty ones") {
    Table values = mynah::flat_toml::parse(R"(
        model_dirs = ["/one", "/two"]
        empty = []
    )");
    REQUIRE(get(values, "model_dirs"));
    CHECK(*get(values, "model_dirs") == SA({"/one", "/two"}));
    REQUIRE(get(values, "empty"));
    CHECK(*get(values, "empty") == SA({}));
}

TEST_CASE("does not split arrays on commas inside quoted strings") {
    Table values = mynah::flat_toml::parse(R"(model_dirs = ["/a,b", "/c"])");
    REQUIRE(get(values, "model_dirs"));
    CHECK(*get(values, "model_dirs") == SA({"/a,b", "/c"}));
}

TEST_CASE("keeps values that carry a trailing comment") {
    // Regression: the RHS reached parse_value with the comment attached, so
    // numeric and bool keys with trailing comments failed to parse and
    // vanished — and the next save re-emitted only what parsed, deleting
    // them from disk.
    Table values = mynah::flat_toml::parse(R"(
        frame_energy = 0.010 # tuned for my mic
        vad = true  # silero
        ai_max_frames = 50 # cap
        vad_alt = true#no-space
    )");
    REQUIRE(get(values, "frame_energy"));
    CHECK(*get(values, "frame_energy") == D(0.010));
    REQUIRE(get(values, "vad"));
    CHECK(*get(values, "vad") == B(true));
    REQUIRE(get(values, "ai_max_frames"));
    CHECK(*get(values, "ai_max_frames") == I(50));
    REQUIRE(get(values, "vad_alt"));
    CHECK(*get(values, "vad_alt") == B(true));
}

TEST_CASE("does not mistake a # inside a value for a comment") {
    Table a = mynah::flat_toml::parse(R"(p = "say #1 loudly")");
    REQUIRE(get(a, "p"));
    CHECK(*get(a, "p") == S("say #1 loudly"));

    Table b = mynah::flat_toml::parse(R"(d = ["/a#b", "/c"])");
    REQUIRE(get(b, "d"));
    CHECK(*get(b, "d") == SA({"/a#b", "/c"}));

    Table c = mynah::flat_toml::parse(R"(p = "say #1" # comment)");
    REQUIRE(get(c, "p"));
    CHECK(*get(c, "p") == S("say #1"));

    Table d = mynah::flat_toml::parse(R"(p = "quote \" then #hash")");
    CHECK(d.size() == 1);
    REQUIRE(get(d, "p"));
    CHECK(*get(d, "p") == S(R"(quote " then #hash)"));
}

TEST_CASE("a commented key survives a parse/emit round trip") {
    // The end-to-end failure this guards: a hand-edited config with an
    // annotated value, saved by a settings UI.
    Table parsed = mynah::flat_toml::parse(R"(
        ai_model = "qwen3.5:9b"
        frame_energy = 0.010 # tuned
        vad = true # silero
        ocr_min_width = 1920
    )");
    Table round = mynah::flat_toml::parse(mynah::flat_toml::emit(parsed));
    CHECK(round.size() == 4);
    REQUIRE(get(round, "frame_energy"));
    CHECK(*get(round, "frame_energy") == D(0.010));
    REQUIRE(get(round, "vad"));
    CHECK(*get(round, "vad") == B(true));
    REQUIRE(get(round, "ocr_min_width"));
    CHECK(*get(round, "ocr_min_width") == I(1920));
}

TEST_CASE("skips comments, blank lines and table headers") {
    Table values = mynah::flat_toml::parse(R"(
        # a comment
        [section]

        language = "uk"
    )");
    CHECK(values.size() == 1);
    REQUIRE(get(values, "language"));
    CHECK(*get(values, "language") == S("uk"));
}

TEST_CASE("round-trips through emit unchanged") {
    Table original{
        {"language", S("ru")},
        {"prompt", S(R"(quote " and slash \)")},
        {"idle_timeout", D(45.0)},
        {"menu_bar", B(true)},
        {"model_dirs", SA({"/one", "/two"})},
    };
    Table round = mynah::flat_toml::parse(mynah::flat_toml::emit(original));
    CHECK(round == original);
}

TEST_CASE("a multi-line prompt survives a full parse/emit round trip (C1)") {
    Table original{{"prompt", Value(std::string("Отвечай кратко.\nПервая строка.\r\nВторая."))}};
    Table parsed = mynah::flat_toml::parse(mynah::flat_toml::emit(original));
    CHECK(parsed == original);
    // Stable on the second pass too.
    CHECK(mynah::flat_toml::parse(mynah::flat_toml::emit(parsed)) == original);
}

// --- the tuning contract's shape (mirrors TuningTests.swift) --------------

TEST_CASE("multi-line arrays parse, skipping comments inside") {
    // tuning.toml carries the hallucination phrase list as a multi-line array
    // with a comment inside it. A strictly line-based parser drops the key —
    // and with it every pin against the file.
    Table values = mynah::flat_toml::parse(R"(
        phrases = [
            "alpha",
            # a comment inside the array
            "beta",
        ]
        next_key = 3
    )");
    REQUIRE(get(values, "phrases"));
    CHECK(*get(values, "phrases") == SA({"alpha", "beta"}));
    REQUIRE(get(values, "next_key"));
    CHECK(*get(values, "next_key") == I(3));
}

TEST_CASE("an unterminated array drops only its own key") {
    Table values = mynah::flat_toml::parse(R"(
        broken = [
        next_key = 7
    )");
    CHECK(get(values, "broken") == nullptr);
    REQUIRE(get(values, "next_key"));
    CHECK(*get(values, "next_key") == I(7));
}

TEST_CASE("a multi-line array closing on the first line is untouched") {
    Table values = mynah::flat_toml::parse("model_dirs = [\"/one\", \"/two\"]\nplain = 1\n");
    REQUIRE(get(values, "model_dirs"));
    CHECK(*get(values, "model_dirs") == SA({"/one", "/two"}));
    REQUIRE(get(values, "plain"));
    CHECK(*get(values, "plain") == I(1));
}
// --- numbers are locale-independent ----------------------------------------

namespace {

// Switches the process to a locale whose decimal separator is a comma, and
// back on destruction. Which ones exist depends on the machine, so a few are
// tried; `ok()` is false when none is installed.
class CommaLocale {
public:
    CommaLocale() {
        const char* current = std::setlocale(LC_ALL, nullptr);
        previous_ = current ? current : "C";
        for (const char* name : {"ru_RU.UTF-8", "uk_UA.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8"}) {
            if (std::setlocale(LC_ALL, name) && std::localeconv()->decimal_point[0] == ',') {
                ok_ = true;
                return;
            }
        }
        std::setlocale(LC_ALL, previous_.c_str());
    }
    ~CommaLocale() { std::setlocale(LC_ALL, previous_.c_str()); }
    CommaLocale(const CommaLocale&) = delete;
    CommaLocale& operator=(const CommaLocale&) = delete;
    bool ok() const { return ok_; }

private:
    std::string previous_;
    bool ok_ = false;
};

} // namespace

TEST_CASE("numbers read and write the same under a comma-decimal locale") {
    // A Qt front end calls setlocale(LC_ALL, "") at startup, so a Russian or
    // Ukrainian desktop runs the core with ',' as the decimal separator. Before
    // this was fixed every float key was dropped on read, and emit wrote
    // `frame_energy = 0,02.0`, which no reader of the file accepts.
    CommaLocale locale;
    if (!locale.ok()) {
        MESSAGE("no comma-decimal locale installed; skipping");
        return;
    }

    Table values = mynah::flat_toml::parse("frame_energy = 0.01\nidle_timeout = 45.0\n");
    REQUIRE(get(values, "frame_energy"));
    CHECK(*get(values, "frame_energy") == D(0.01));
    REQUIRE(get(values, "idle_timeout"));
    CHECK(*get(values, "idle_timeout") == D(45.0));

    std::string text = mynah::flat_toml::emit(Table{{"frame_energy", D(0.02)}});
    CHECK(text == "frame_energy = 0.02\n");
    CHECK(mynah::flat_toml::parse(text) == Table{{"frame_energy", D(0.02)}});

    // The process locale is still the one the front end chose.
    CHECK(std::localeconv()->decimal_point[0] == ',');
}
