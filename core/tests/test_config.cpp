// The config module, ported from MynahConfigTests in
// macos/Tests/MynahAppTests/ConfigTests.swift with the Python suite's
// legacy-import cases added. The file on disk is co-owned by the core, the
// Python CLI and the Swift app while the migration runs; these tests are what
// keeps the three of them reading and writing one file without losing each
// other's keys.

#include "vendor/doctest.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include <sys/stat.h>

#include "config/config.hpp"
#include "config/flat_toml.hpp"
#include "env.hpp"

using mynah::config::Config;
using mynah::flat_toml::Table;
using mynah::flat_toml::Value;
using mynah_test::EnvOverride;
using mynah_test::TmpDir;
using mynah_test::write_file;

TEST_CASE("defaults match mynah/config.py and MynahConfig.swift") {
    Config config;
    CHECK(config.language == "ru");
    CHECK(config.hotkey == "<cmd>+<shift>+.");
    CHECK(config.trigger == "toggle");
    CHECK(config.idle_timeout == 45.0);
    CHECK(config.auto_stop_silence == 10.0);
    CHECK(config.vad);
    CHECK(config.show_indicator);
    CHECK(!config.idle_visible);
    // P5: injector stays, the Python-only keys are gone.
    CHECK(config.injector.empty());
    CHECK(config.model.empty());
    CHECK(config.prompt.empty());
}

TEST_CASE("reads an int where a float is expected") {
    // `mynah set idle_timeout=0` writes a bare `0`.
    Config config = mynah::config::from_values(
        mynah::flat_toml::parse("idle_timeout = 0"));
    CHECK(config.idle_timeout == 0);
}

TEST_CASE("falls back to defaults for missing and mistyped keys") {
    Config config = mynah::config::from_values(mynah::flat_toml::parse(R"(vad = "yes")"));
    CHECK(config.vad); // default, not a crash
    CHECK(config.language == "ru");
}

TEST_CASE("transcription_mode normalises: on_stop is kept, anything else is live") {
    Config on_stop = mynah::config::from_values(
        mynah::flat_toml::parse("transcription_mode = \"on_stop\""));
    CHECK(on_stop.transcription_mode == "on_stop");

    Config live = mynah::config::from_values(
        mynah::flat_toml::parse("transcription_mode = \"live\""));
    CHECK(live.transcription_mode == "live");

    // A hand-edited bogus value must not silently change behaviour: it
    // reads as the default.
    Config bogus = mynah::config::from_values(
        mynah::flat_toml::parse("transcription_mode = \"whenever\""));
    CHECK(bogus.transcription_mode == "live");

    Config absent; // the default
    CHECK(absent.transcription_mode == "live");
}

TEST_CASE("default_path honours MYNAH_CONFIG_DIR, or ignores it when empty") {
    // The baseline with the variable absent — captured first, restored after.
    std::filesystem::path baseline;
    if (const char* old = std::getenv("MYNAH_CONFIG_DIR")) {
        std::string old_value = old;
        unsetenv("MYNAH_CONFIG_DIR");
        baseline = mynah::config::default_path();
        setenv("MYNAH_CONFIG_DIR", old_value.c_str(), 1);
    } else {
        baseline = mynah::config::default_path();
    }

    // An empty override is ignored, exactly as the Swift side ignores one.
    {
        EnvOverride empty_dir("MYNAH_CONFIG_DIR", "");
        CHECK(mynah::config::default_path() == baseline);
    }
    {
        EnvOverride set_dir("MYNAH_CONFIG_DIR", "/tmp/mynah-core-test-profile");
        CHECK(mynah::config::default_path() ==
              std::filesystem::path("/tmp/mynah-core-test-profile/config.toml"));
    }
}

TEST_CASE("an absent config file means defaults, with no error") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    mynah::config::ReadResult read = mynah::config::read_file(mynah::config::default_path());
    CHECK(read.status == mynah::config::ReadStatus::missing);
    CHECK(read.config == Config{});
    CHECK(read.message.empty());
}

TEST_CASE("an unreadable config file still yields defaults, but reports the failure") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    // A directory where the config file should be reads as EISDIR — a real
    // read failure, not "no file yet".
    std::filesystem::path path = mynah::config::default_path();
    std::filesystem::create_directories(path);
    mynah::config::ReadResult read = mynah::config::read_file(path);
    CHECK(read.status == mynah::config::ReadStatus::unreadable);
    CHECK(read.config == Config{}); // the app still works on defaults
    CHECK(read.message.find(path.string()) != std::string::npos);

    // And load() raises on it, as mynah/config.py does.
    CHECK_THROWS_AS(mynah::config::load(), mynah::config::Error);
}

TEST_CASE("a valid file loads cleanly with no error") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    write_file(mynah::config::default_path(), R"(language = "uk"
vad = false
)");
    mynah::config::ReadResult read = mynah::config::read_file(mynah::config::default_path());
    CHECK(read.status == mynah::config::ReadStatus::ok);
    CHECK(read.message.empty());
    CHECK(read.config.language == "uk");
    CHECK(!read.config.vad);
}

TEST_CASE("saving preserves keys owned by the other writers") {
    // `stt_provider` is a real Python-CLI key the core has no field for
    // (P5 dropped it); `future_setting` stands in for a key written by a
    // front end this core has never heard of.
    Table values = mynah::flat_toml::parse(R"(
        stt_provider = "mlx"
        future_setting = ["one", "two"]
        language = "ru"
    )");

    Config config = mynah::config::from_values(values);
    config.language = "uk";
    mynah::config::merge_into(values, config);

    CHECK(mynah::flat_toml::str("mlx") == values["stt_provider"]);
    REQUIRE(std::holds_alternative<std::vector<std::string>>(values["future_setting"]));
    CHECK(std::get<std::vector<std::string>>(values["future_setting"]) ==
          std::vector<std::string>{"one", "two"});
    CHECK(mynah::flat_toml::str("uk") == values["language"]);
}

TEST_CASE("save writes the file, and re-reading it returns what was saved") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    std::filesystem::path path = mynah::config::default_path();

    // A file another writer made, with keys of its own.
    write_file(path, "stt_provider = \"mlx\"\nlanguage = \"ru\"\n");

    Config config = mynah::config::read_file(path).config;
    config.language = "uk";
    config.frame_energy = 0.02;
    mynah::config::save(config);

    // The other writer's key is still on disk, byte-stable.
    std::ifstream in(path);
    std::string on_disk((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    CHECK(on_disk.find("stt_provider = \"mlx\"") != std::string::npos);

    mynah::config::ReadResult reread = mynah::config::read_file(path);
    CHECK(reread.status == mynah::config::ReadStatus::ok);
    CHECK(reread.config.language == "uk");
    CHECK(reread.config.frame_energy == 0.02);
}

TEST_CASE("first run imports the dictate_* keys from whiz's config") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    TmpDir legacy_dir;
    std::filesystem::path legacy = legacy_dir.path() / "config.toml";
    write_file(legacy, R"(
        ai_model = "qwen3.5:9b"
        dictate_language = "uk"
        dictate_hotkey = "<f8>"
        dictate_idle_timeout = 0
        dictate_vad = false
        dictate_not_a_mynah_key = 1
    )");
    EnvOverride override_legacy("MYNAH_LEGACY_CONFIG", legacy.string());

    Config config = mynah::config::load();
    CHECK(config.language == "uk");
    CHECK(config.hotkey == "<f8>");
    CHECK(config.idle_timeout == 0); // int in whiz's file, float here
    CHECK(!config.vad);

    // The import is kept: the file exists now, and says what load() said.
    std::filesystem::path path = mynah::config::default_path();
    REQUIRE(std::filesystem::exists(path));
    mynah::config::ReadResult read = mynah::config::read_file(path);
    CHECK(read.status == mynah::config::ReadStatus::ok);
    CHECK(read.config.language == "uk");
    CHECK(read.config.hotkey == "<f8>");
}

TEST_CASE("the whiz import is skipped once a config exists") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    std::filesystem::path path = mynah::config::default_path();
    write_file(path, "language = \"ru\"\n");

    TmpDir legacy_dir;
    std::filesystem::path legacy = legacy_dir.path() / "config.toml";
    write_file(legacy, "dictate_language = \"uk\"\n");
    EnvOverride override_legacy("MYNAH_LEGACY_CONFIG", legacy.string());

    Config config = mynah::config::load();
    CHECK(config.language == "ru");
}

TEST_CASE("a mistyped whiz value is skipped, not trusted") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    TmpDir legacy_dir;
    std::filesystem::path legacy = legacy_dir.path() / "config.toml";
    write_file(legacy, "dictate_vad = \"yes\"\ndictate_language = \"uk\"\n");
    EnvOverride override_legacy("MYNAH_LEGACY_CONFIG", legacy.string());

    Config config = mynah::config::load();
    CHECK(config.vad);    // the default; "yes" is not a bool
    CHECK(config.language == "uk");
}
namespace {

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("saving through a symlinked config writes the target and keeps the link") {
    // A dotfiles manager (stow, chezmoi…) links ~/.config/mynah/config.toml
    // into a repository. Renaming over the link would detach it: the real file
    // stays unchanged and the link becomes a plain file.
    TmpDir dir;
    TmpDir dotfiles;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    std::filesystem::path target = dotfiles.path() / "mynah.toml";
    write_file(target, "language = \"uk\"\nstt_provider = \"mlx\"\n");

    SUBCASE("absolute link") {
        std::filesystem::create_symlink(target, mynah::config::default_path());
    }
    SUBCASE("relative link") {
        std::filesystem::create_symlink(
            std::filesystem::relative(target, dir.path()), mynah::config::default_path());
    }

    Config config = mynah::config::read_file(mynah::config::default_path()).config;
    REQUIRE(config.language == "uk");
    config.language = "en";
    mynah::config::save(config);

    CHECK(std::filesystem::is_symlink(mynah::config::default_path()));
    std::string on_disk = read_all(target);
    CHECK(on_disk.find("language = \"en\"") != std::string::npos);
    CHECK(on_disk.find("stt_provider = \"mlx\"") != std::string::npos);
}

TEST_CASE("save leaves no temp file behind and keeps the file's permissions") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    std::filesystem::path path = mynah::config::default_path();
    write_file(path, "language = \"ru\"\n");
    REQUIRE(::chmod(path.c_str(), 0600) == 0);

    mynah::config::save(Config{});

    struct stat after {};
    REQUIRE(::stat(path.c_str(), &after) == 0);
    CHECK((after.st_mode & 0777) == 0600);

    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        (void)entry;
        ++entries;
    }
    CHECK(entries == 1); // config.toml, and nothing else
}

TEST_CASE("a first save creates the config with the usual 0644") {
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", (dir.path() / "fresh").string());
    mynah::config::save(Config{});

    struct stat created {};
    REQUIRE(::stat(mynah::config::default_path().c_str(), &created) == 0);
    CHECK((created.st_mode & 0777) == 0644);
}

TEST_CASE("without a whiz config, creating the default config writes nothing") {
    // Guards the test environment itself (tests/main.cpp points
    // MYNAH_LEGACY_CONFIG at a file that does not exist): the first-run import
    // must never read the developer's real ~/.config/whiz/config.toml.
    TmpDir dir;
    EnvOverride override_dir("MYNAH_CONFIG_DIR", dir.path().string());
    Config config = mynah::config::load();
    CHECK(config == Config{});
    CHECK(!std::filesystem::exists(mynah::config::default_path()));
}
