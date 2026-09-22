// The model downloader (linux/common/downloads.cpp), exercised offline
// through file:// sources so the suite needs no network.
//
// Regression pinned here: the body callback was registered with the FILE*
// as its userdata but read it as a FetchContext — every chunk poked the
// stdio struct and nothing was ever written, so `mynah models download`
// could not produce a model file at all.

#include "vendor/doctest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "downloads.hpp"
#include "env.hpp"

namespace {

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A deterministic 300 KB "model", bigger than any single curl chunk so the
// body callback runs many times.
std::string fake_model() {
    std::string data(300000, '\0');
    std::uint32_t x = 2463534242u;
    for (char& c : data) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = char(x & 0xFF);
    }
    return data;
}

} // namespace

TEST_CASE("file_sha256 matches a known vector") {
    mynah_test::TmpDir dir;
    mynah_test::write_file(dir.path() / "abc", "abc");
    CHECK(mynah::download::file_sha256((dir.path() / "abc").string()) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("a download lands byte for byte, with progress, and no .part left") {
    mynah_test::TmpDir dir;
    const std::string payload = fake_model();
    mynah_test::write_file(dir.path() / "source.bin", payload);
    const std::string url = "file://" + (dir.path() / "source.bin").string();

    std::uint64_t last_progress = 0;
    int progress_calls = 0;
    mynah::download::Result result = mynah::download::fetch(
        url, (dir.path() / "models").string(), "ggml-fake.bin", "",
        [&](std::uint64_t received, std::uint64_t) {
            ++progress_calls;
            last_progress = received;
            return true;
        });

    REQUIRE_MESSAGE(result.ok, result.error);
    CHECK(read_all(result.path) == payload);
    CHECK(progress_calls > 0);
    CHECK(last_progress == payload.size());
    CHECK(!std::filesystem::exists(dir.path() / "models" / "ggml-fake.bin.part"));
}

TEST_CASE("a matching SHA-256 is accepted") {
    mynah_test::TmpDir dir;
    const std::string payload = fake_model();
    mynah_test::write_file(dir.path() / "source.bin", payload);
    const std::string expected = mynah::download::file_sha256((dir.path() / "source.bin").string());

    mynah::download::Result result = mynah::download::fetch(
        "file://" + (dir.path() / "source.bin").string(), (dir.path() / "models").string(),
        "ggml-fake.bin", expected, nullptr);
    REQUIRE_MESSAGE(result.ok, result.error);
    CHECK(read_all(result.path) == payload);
}

TEST_CASE("a wrong SHA-256 is refused and never lands under the model's name") {
    mynah_test::TmpDir dir;
    mynah_test::write_file(dir.path() / "source.bin", fake_model());

    mynah::download::Result result = mynah::download::fetch(
        "file://" + (dir.path() / "source.bin").string(), (dir.path() / "models").string(),
        "ggml-fake.bin", std::string(64, '0'), nullptr);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("SHA-256 mismatch") != std::string::npos);
    CHECK(!std::filesystem::exists(dir.path() / "models" / "ggml-fake.bin"));
    CHECK(!std::filesystem::exists(dir.path() / "models" / "ggml-fake.bin.part"));
}

TEST_CASE("a missing source fails cleanly") {
    mynah_test::TmpDir dir;
    mynah::download::Result result = mynah::download::fetch(
        "file://" + (dir.path() / "nothing-here.bin").string(), (dir.path() / "models").string(),
        "ggml-fake.bin", "", nullptr);
    CHECK_FALSE(result.ok);
    CHECK(!result.error.empty());
    CHECK(!std::filesystem::exists(dir.path() / "models" / "ggml-fake.bin"));
    CHECK(!std::filesystem::exists(dir.path() / "models" / "ggml-fake.bin.part"));
}

TEST_CASE("a cancelled download leaves nothing behind") {
    mynah_test::TmpDir dir;
    mynah_test::write_file(dir.path() / "source.bin", fake_model());
    mynah::download::Result result = mynah::download::fetch(
        "file://" + (dir.path() / "source.bin").string(), (dir.path() / "models").string(),
        "ggml-fake.bin", "", [](std::uint64_t, std::uint64_t) { return false; });
    CHECK_FALSE(result.ok);
    CHECK(result.error == "cancelled");
    CHECK(!std::filesystem::exists(dir.path() / "models" / "ggml-fake.bin"));
    CHECK(!std::filesystem::exists(dir.path() / "models" / "ggml-fake.bin.part"));
}

TEST_CASE("Content-Length is read in any case, and a redirect's does not stick") {
    using mynah::download::detail::header_total;
    std::uint64_t total = 0;
    header_total("Content-Length: 1234\r\n", total);
    CHECK(total == 1234);
    // HTTP/2 spells header names in lowercase — HuggingFace and its CDN do.
    header_total("HTTP/2 200\r\n", total);
    CHECK(total == 0);
    header_total("content-length: 487601967\r\n", total);
    CHECK(total == 487601967);
    // Following a redirect: the 302's own length is dropped at the next
    // status line, and only the file's counts.
    header_total("HTTP/2 302\r\n", total);
    header_total("content-length: 1170\r\n", total);
    header_total("HTTP/2 200\r\n", total);
    CHECK(total == 0);
    header_total("content-length: 885098\r\n", total);
    CHECK(total == 885098);
    // Not the header, or not a number: unchanged.
    header_total("content-type: application/octet-stream\r\n", total);
    header_total("Content-Length: lots\r\n", total);
    CHECK(total == 885098);
}
