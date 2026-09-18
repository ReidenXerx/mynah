// Model alias resolution — the twin of
// macos/Tests/MynahAppTests/AliasResolutionTests.swift (and
// tests/test_models.py before it). The same `model` value must mean the
// same model on every platform: full aliases, short aliases, prefixes,
// the unquantized-first preference, and discovery that ignores files that
// are not ggml models.

#include "vendor/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "env.hpp"
#include "models/resolve.hpp"

using mynah::models::alias_from_filename;
using mynah::models::resolve;
using mynah_test::TmpDir;

namespace {

// A temp directory with empty .bin files for the given model filenames.
std::pair<TmpDir, std::vector<std::filesystem::path>> make_disk(
    std::initializer_list<const char*> filenames) {
    TmpDir dir;
    std::vector<std::filesystem::path> dirs{dir.path()};
    for (const char* name : filenames) {
        std::filesystem::path file = dir.path() / name;
        std::ofstream out(file, std::ios::binary);
        out << "x";
    }
    return {std::move(dir), std::move(dirs)};
}

std::string filename_of(const std::filesystem::path& path) {
    return path.empty() ? std::string() : path.filename().string();
}

} // namespace

TEST_CASE("resolves a full alias like large-v3-turbo-q5_0") {
    auto [dir, dirs] = make_disk({"ggml-large-v3-turbo-q5_0.bin"});
    CHECK(filename_of(resolve("large-v3-turbo-q5_0", dirs)) ==
          "ggml-large-v3-turbo-q5_0.bin");
}

TEST_CASE("resolves a filename written in config as if it were an alias") {
    // The settings UI shows filenames (ggml-medium.bin); a user copying
    // that into the config must not get "no model found".
    auto [dir, dirs] = make_disk({"ggml-medium.bin"});
    CHECK(filename_of(resolve("ggml-medium.bin", dirs)) == "ggml-medium.bin");
}

TEST_CASE("short alias 'turbo' resolves when exactly one turbo variant exists") {
    auto [dir, dirs] = make_disk({"ggml-large-v3-turbo-q5_0.bin"});
    CHECK(filename_of(resolve("turbo", dirs)) == "ggml-large-v3-turbo-q5_0.bin");
}

TEST_CASE("short alias 'turbo' is ambiguous with two variants — resolves to nothing") {
    auto [dir, dirs] =
        make_disk({"ggml-large-v3-turbo.bin", "ggml-large-v3-turbo-q5_0.bin"});
    CHECK(resolve("turbo", dirs).empty());
}

TEST_CASE("prefix 'large-v3' picks the best on-disk variant by preference order") {
    auto [dir, dirs] = make_disk({"ggml-large-v3.bin", "ggml-large-v3-q5_0.bin"});
    CHECK(filename_of(resolve("large-v3", dirs)) == "ggml-large-v3.bin");
}

TEST_CASE("prefix 'large-v3' resolves a quantized-only disk") {
    auto [dir, dirs] = make_disk({"ggml-large-v3-q5_0.bin"});
    CHECK(filename_of(resolve("large-v3", dirs)) == "ggml-large-v3-q5_0.bin");
}

TEST_CASE("prefix never rewrites an explicit quantized request") {
    auto [dir, dirs] =
        make_disk({"ggml-large-v3-turbo.bin", "ggml-large-v3-turbo-q5_0.bin"});
    CHECK(filename_of(resolve("large-v3-turbo-q5_0", dirs)) ==
          "ggml-large-v3-turbo-q5_0.bin");
}

TEST_CASE("an unknown name resolves to nothing, not to a fallback") {
    auto [dir, dirs] = make_disk({"ggml-large-v3.bin"});
    CHECK(resolve("nonexistent-model", dirs).empty());
}

TEST_CASE("auto-pick (empty configured) walks the full five-class preference") {
    // The old Swift list stopped at medium; a disk holding only small/base
    // models auto-picked nothing. Pin entry-for-entry equality with the
    // current eleven-entry list, then a small-only disk.
    const std::vector<std::string> preference{
        "ggml-large-v3-turbo.bin",   "ggml-large-v3-turbo-q8_0.bin",
        "ggml-large-v3-turbo-q5_0.bin", "ggml-large-v3.bin",
        "ggml-large-v3-q5_0.bin",    "ggml-medium.bin",
        "ggml-medium-q5_0.bin",      "ggml-small.bin",
        "ggml-small-q5_0.bin",       "ggml-base.bin",
        "ggml-base-q5_0.bin",
    };
    for (std::size_t i = 0; i < preference.size(); ++i)
        CHECK(alias_from_filename(preference[i]) ==
              alias_from_filename(mynah::models::kPreference[i]));

    auto [dir, dirs] = make_disk({"ggml-small.bin"});
    CHECK(filename_of(resolve("", dirs)) == "ggml-small.bin");
}

TEST_CASE("alias_from_filename mirrors _alias_from_name") {
    CHECK(alias_from_filename("ggml-large-v3-turbo-q5_0.bin") == "large-v3-turbo-q5_0");
    CHECK(alias_from_filename("ggml-medium.bin") == "medium");
    CHECK(alias_from_filename("foo.bin") == "foo");
    CHECK(alias_from_filename("ggml-foo") == "foo");
}

TEST_CASE("non-ggml and non-bin files are not discovered as aliases") {
    auto [dir, dirs] = make_disk({"ggml-medium.bin", "random.bin", "not-a-model.txt"});
    CHECK(filename_of(resolve("medium", dirs)) == "ggml-medium.bin");
    CHECK(resolve("random", dirs).empty());
    CHECK(resolve("not-a-model", dirs).empty());
}

TEST_CASE("an explicit path wins over the search directories") {
    auto [dir, dirs] = make_disk({"ggml-small.bin"});
    std::filesystem::path explicit_file = dir.path() / "my-model.bin";
    {
        std::ofstream out(explicit_file, std::ios::binary);
        out << "x";
    }
    CHECK(resolve(explicit_file.string(), dirs) == explicit_file);
}
TEST_CASE("MYNAH_MODEL_DIR is searched first, as it is in the Python engine") {
    auto [dir, _] = make_disk({"ggml-small.bin"});
    mynah_test::EnvOverride override_dir("MYNAH_MODEL_DIR", dir.path().string());

    std::vector<std::filesystem::path> dirs = mynah::models::search_directories();
    REQUIRE(!dirs.empty());
    CHECK(dirs.front() == dir.path());
    // And it resolves through the real search path, not just a passed-in list.
    CHECK(filename_of(mynah::models::resolve("small", dirs)) == "ggml-small.bin");
}

TEST_CASE("the search path carries this platform's conventional directories") {
    // Phase 3 depends on the Linux ones being here: a model the Python
    // engine found under ~/.local/share/mynah/models or the distro's
    // /usr/share/whisper.cpp must still be found after the cutover.
    mynah_test::EnvOverride no_override("MYNAH_MODEL_DIR", "");
    std::vector<std::filesystem::path> dirs = mynah::models::search_directories();
    auto has = [&](const std::string& suffix) {
        for (const auto& dir : dirs)
            if (dir.string().find(suffix) != std::string::npos) return true;
        return false;
    };
    CHECK(has(".cache/whisper")); // shared by every platform
#if defined(__APPLE__)
    CHECK(has("Library/Caches/whisper"));
#else
    CHECK(has(".local/share/mynah/models"));
    CHECK(has("/usr/share/whisper.cpp"));
#endif
}
