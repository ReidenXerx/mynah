// The injectors: the port of the injector tests in tests/test_linux.py.
// Everything that touches wtype, wl-clipboard or hyprctl resolves through
// Tools — so the tests point at little fake scripts they write themselves,
// and read back what the fakes recorded.

#include "vendor/doctest.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "env.hpp"
#include "injector.hpp"

namespace {

using mynah::inject::Injector;
using mynah::inject::Tools;

// A fake binary: a shell script that records its command line — the path
// it was run as, then its arguments — to `record_path` (one call per line)
// and its stdin to `stdin_path`. `exit_code` scripts the failure path.
//
// The path is recorded ("$0 $*", not just "$*") because every fake writes
// to the same file: without it, a line cannot say which tool was run, and
// the tests that ask "did wl-copy get called?" can never pass.
std::string make_fake(const std::filesystem::path& base, const std::string& name,
                      const std::string& record_path, const std::string& stdin_path,
                      const std::string& extra = "", int exit_code = 0) {
    std::filesystem::path script = base / name;
    std::string body = "#!/bin/sh\n";
    body += "printf '%s\\n' \"$0 $*\" >> " + record_path + "\n";
    body += "cat > " + (stdin_path.empty() ? std::string("/dev/null") : stdin_path) + "\n";
    if (!extra.empty()) body += extra + "\n";
    body += "exit " + std::to_string(exit_code) + "\n";
    std::ofstream out(script);
    out << body;
    ::chmod(script.c_str(), 0755);
    return script.string();
}

std::vector<std::string> recorded(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
        if (!line.empty()) lines.push_back(line);
    return lines;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

struct Fakes {
    mynah_test::TmpDir dir;
    std::string record; // argv records
    std::string wtype_stdin;
    std::string copy_stdin;

    std::string wtype;
    std::string wl_copy;
    std::string wl_paste;
    std::string hyprctl;

    explicit Fakes() {
        record = (dir.path() / "records").string();
        wtype_stdin = (dir.path() / "wtype-stdin").string();
        copy_stdin = (dir.path() / "wl-copy-stdin").string();
        wtype = make_fake(dir.path(), "wtype", record, wtype_stdin);
        wl_copy = make_fake(dir.path(), "wl-copy", record, copy_stdin);
        hyprctl = make_fake(dir.path(), "hyprctl", record, "");
        // wl-paste has state: its behavior is scripted per test via env.
        wl_paste = make_fake(dir.path(), "wl-paste", record, "");
    }
};

Tools tools_with(const Fakes& fakes) {
    Tools tools;
    tools.wtype = fakes.wtype;
    tools.wl_copy = fakes.wl_copy;
    tools.wl_paste = fakes.wl_paste;
    tools.hyprctl = fakes.hyprctl;
    return tools;
}

// Whether the fake clipboard holds `expected` within 1.5 s.
bool clipboard_becomes(const Fakes& fakes, const std::string& expected) {
    auto began = std::chrono::steady_clock::now();
    while (read_file(fakes.copy_stdin) != expected) {
        if (std::chrono::steady_clock::now() - began > std::chrono::milliseconds(1500))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

// A wl-paste that serves whatever the fake wl-copy last wrote — the
// clipboard as the next read really sees it — or the user's text before
// anything was copied.
std::string make_live_paste(const Fakes& fakes) {
    std::string script = (fakes.dir.path() / "wl-paste-live").string();
    std::ofstream out(script);
    out << "#!/bin/sh\n";
    out << "cat > /dev/null\n";
    out << "if [ \"$1\" = --list-types ]; then echo text/plain; exit 0; fi\n";
    out << "if [ -f " << fakes.copy_stdin << " ]; then cat " << fakes.copy_stdin
        << "; else printf 'the users text'; fi\n";
    out.close();
    ::chmod(script.c_str(), 0755);
    return script;
}

} // namespace

TEST_CASE("the text goes in on stdin, never in argv") {
    Fakes fakes;
    auto injector = mynah::inject::make_wtype(tools_with(fakes));
    REQUIRE(injector->type_text("привет мир"));
    auto argv = recorded(fakes.record);
    REQUIRE(argv.size() == 1);
    CHECK(argv[0] == fakes.wtype + " -"); // the text is NOT on this line
    CHECK(read_file(fakes.wtype_stdin) == "привет мир");
}

TEST_CASE("the child gets stdin, stdout and stderr — and nothing else") {
    // Regression: the child inherited both ends of its own stdin pipe, so a
    // tool that reads stdin to EOF — `wtype -`, `wl-copy`, the `cat` in
    // these fakes — waited forever for an EOF its own descriptor prevented,
    // and the first utterance hung dictation for good. It also carried the
    // engine's sockets into wl-copy's long-lived daemon.
    Fakes fakes;
    std::string leaks = (fakes.dir.path() / "leaked-fds").string();
    std::string probe = (fakes.dir.path() / "wtype-probe").string();
    {
        std::ofstream out(probe);
        out << "#!/bin/sh\n";
        out << "cat > /dev/null\n"; // reads stdin to EOF, like the real wtype
        // 3..9 only: the shell running this probe keeps its own script
        // open at 10 and above (bash, and Arch's /bin/sh is bash), which
        // is not ours to account for. The leaks the bug produced were 3-6.
        out << "for fd in 3 4 5 6 7 8 9; do\n";
        out << "  if ( : <&$fd ) 2>/dev/null || ( : >&$fd ) 2>/dev/null; then echo $fd >> "
            << leaks << "; fi\n";
        out << "done\n";
        out << "exit 0\n";
    }
    ::chmod(probe.c_str(), 0755);
    Tools tools = tools_with(fakes);
    tools.wtype = probe;

    auto began = std::chrono::steady_clock::now();
    CHECK(mynah::inject::make_wtype(tools)->type_text("привет"));
    // Well under the 10 s kill timeout: EOF arrived as soon as we sent.
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(5));
    CHECK(read_file(leaks).empty());
}

TEST_CASE("a tool that exits without reading stdin does not kill us") {
    // wtype on a compositor without the virtual-keyboard protocol exits at
    // once. Writing the text to a reader that is gone must be an error we
    // see, not a SIGPIPE that ends the daemon. The text is large enough to
    // outlast any socket buffer, so the write really does hit the closed end.
    Fakes fakes;
    std::string quitter = (fakes.dir.path() / "wtype-quits").string();
    {
        std::ofstream out(quitter);
        out << "#!/bin/sh\nexit 1\n";
    }
    ::chmod(quitter.c_str(), 0755);
    Tools tools = tools_with(fakes);
    tools.wtype = quitter;

    std::string big(1 << 20, 'x'); // 1 MiB
    CHECK_FALSE(mynah::inject::make_wtype(tools)->type_text(big));
    // Reaching this line is the assertion: SIGPIPE would have killed the
    // test binary before it.
}

TEST_CASE("an empty text does not spawn wtype") {
    Fakes fakes;
    auto injector = mynah::inject::make_wtype(tools_with(fakes));
    CHECK(injector->type_text(""));
    CHECK(recorded(fakes.record).empty());
}

TEST_CASE("a failing wtype is reported, not swallowed") {
    Fakes fakes;
    std::string record = (fakes.dir.path() / "records").string();
    std::string failing = make_fake(fakes.dir.path(), "wtype-fail", record, "", "", 3);
    Tools tools = tools_with(fakes);
    tools.wtype = failing;
    auto injector = mynah::inject::make_wtype(tools);
    CHECK_FALSE(injector->type_text("привет"));
}

TEST_CASE("a missing wtype names the remedy") {
    // In a Wayland session — as Python's twin sets up — or the X11 refusal
    // answers first and this passes only when run inside a live session.
    mynah_test::EnvOverride wayland("WAYLAND_DISPLAY", "wayland-0");
    Tools tools; // nothing installed
    auto injector = mynah::inject::make_wtype(tools);
    auto [ok, remedy] = injector->check();
    CHECK_FALSE(ok);
    CHECK(remedy.find("wtype is not installed") != std::string::npos);
    CHECK(remedy.find("sudo pacman -S wtype") != std::string::npos);
    CHECK(remedy.find("sudo apt install wtype") != std::string::npos);
    CHECK_FALSE(injector->type_text("привет"));
}

TEST_CASE("an X11 session is refused with the reason") {
    mynah_test::EnvOverride no_wayland("WAYLAND_DISPLAY", "");
    mynah_test::EnvOverride x11("XDG_SESSION_TYPE", "x11");
    Fakes fakes;
    auto injector = mynah::inject::make_wtype(tools_with(fakes));
    auto [ok, remedy] = injector->check();
    CHECK_FALSE(ok);
    CHECK(remedy.find("Not a Wayland session") != std::string::npos);
    CHECK(remedy.find("does not ship an X11 path") != std::string::npos);
}

TEST_CASE("terminals paste with Ctrl+Shift+V; everything else with Ctrl+V") {
    for (const char* terminal : {"foot", "kitty", "alacritty", "org.wezfurlong.wezterm",
                                 "com.mitchellh.ghostty"}) {
        auto [mods, key] = mynah::inject::paste_chord(terminal);
        CHECK(mods == "CTRL SHIFT");
        CHECK(key == "V");
    }
    auto [mods, key] = mynah::inject::paste_chord("firefox");
    CHECK(mods == "CTRL");
    CHECK(key == "V");
    // Matched as substrings, like the Python side did.
    std::tie(mods, key) = mynah::inject::paste_chord("footclient");
    CHECK(mods == "CTRL SHIFT");
}

TEST_CASE("the clipboard is borrowed and given back") {
    Fakes fakes;
    // wl-paste scripts: --list-types says text/plain; the read returns the
    // user's clipboard.
    std::string paste_script = (fakes.dir.path() / "wl-paste2").string();
    {
        std::ofstream out(paste_script);
        out << "#!/bin/sh\n";
        out << "printf '%s\\n' \"$*\" >> " << fakes.record << "\n";
        out << "cat > /dev/null\n";
        out << "if [ \"$1\" = --list-types ]; then echo text/plain; else printf 'the users text'; "
               "fi\n";
        out << "exit 0\n";
    }
    ::chmod(paste_script.c_str(), 0755);
    Tools tools = tools_with(fakes);
    tools.wl_paste = paste_script;

    auto injector = mynah::inject::make_clipboard(tools);
    REQUIRE(injector->type_text("привет"));

    // Our text landed on the clipboard…
    auto argv = recorded(fakes.record);
    bool wrote_text = false;
    for (const std::string& line : argv)
        if (line.find("wl-copy --type text/plain") != std::string::npos) wrote_text = true;
    CHECK(wrote_text);
    std::string written = read_file(fakes.copy_stdin);

    CHECK(written == "привет");

    // …and within ~1.5 s the original came back. Waited for by content, not
    // by "changed": the fake wl-copy truncates before it writes, so a read
    // in between sees an empty file.
    CHECK(clipboard_becomes(fakes, "the users text"));
}

TEST_CASE("two utterances in a row still give back the user's clipboard") {
    // The second paste reads the clipboard while the first one's text is
    // still on it; restoring what IT saved would lose the user's text.
    Fakes fakes;
    Tools tools = tools_with(fakes);
    tools.wl_paste = make_live_paste(fakes);

    auto injector = mynah::inject::make_clipboard(tools);
    REQUIRE(injector->type_text("первое"));
    REQUIRE(injector->type_text("второе"));
    CHECK(read_file(fakes.copy_stdin) == "второе");
    CHECK(clipboard_becomes(fakes, "the users text"));
}

TEST_CASE("quitting right after a paste still gives the clipboard back") {
    Fakes fakes;
    Tools tools = tools_with(fakes);
    tools.wl_paste = make_live_paste(fakes);

    auto injector = mynah::inject::make_clipboard(tools);
    REQUIRE(injector->type_text("привет"));
    injector.reset(); // waits for the pending restore, and nothing outlives it
    CHECK(read_file(fakes.copy_stdin) == "the users text");
}

TEST_CASE("an image on the clipboard is left alone") {
    Fakes fakes;
    std::string paste_script = (fakes.dir.path() / "wl-paste-image").string();
    {
        std::ofstream out(paste_script);
        out << "#!/bin/sh\n";
        out << "printf '%s\\n' \"$*\" >> " << fakes.record << "\n";
        out << "cat > /dev/null\n";
        out << "if [ \"$1\" = --list-types ]; then echo image/png; fi\n";
        out << "exit 0\n";
    }
    ::chmod(paste_script.c_str(), 0755);
    Tools tools = tools_with(fakes);
    tools.wl_paste = paste_script;

    auto injector = mynah::inject::make_clipboard(tools);
    REQUIRE(injector->type_text("привет"));

    // The paste went through (one wl-copy with our text) but no restore
    // ever followed: an image cannot be put back.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto argv = recorded(fakes.record);
    int copy_calls = 0;
    for (const std::string& line : argv)
        if (line.find("wl-copy") != std::string::npos) ++copy_calls;
    CHECK(copy_calls == 1); // our text only, never the restore
}

TEST_CASE("the chord goes through the compositor when it can") {
    Fakes fakes;
    mynah_test::EnvOverride hyprland("HYPRLAND_INSTANCE_SIGNATURE", "test-signature");
    Tools tools = tools_with(fakes);

    auto injector = mynah::inject::make_clipboard(tools);
    REQUIRE(injector->type_text("привет"));

    auto argv = recorded(fakes.record);
    bool via_compositor = false;
    for (const std::string& line : argv)
        if (line.find("send_shortcut") != std::string::npos &&
            line.find("activewindow") != std::string::npos)
            via_compositor = true;
    CHECK(via_compositor);
}

TEST_CASE("the smart injector pastes only where typing fails") {
    Fakes fakes;
    mynah_test::EnvOverride hyprland("HYPRLAND_INSTANCE_SIGNATURE", "test-signature");

    // hyprctl fakes return the focused class: "warp" (keymap-deaf).
    std::string hyprctl_script = (fakes.dir.path() / "hyprctl-class").string();
    {
        std::ofstream out(hyprctl_script);
        out << "#!/bin/sh\n";
        out << "cat > /dev/null\n";
        out << "if [ \"$2\" = activewindow ]; then printf '{\"class\": \"warp\"}'; fi\n";
        out << "exit 0\n";
    }
    ::chmod(hyprctl_script.c_str(), 0755);
    Tools warp_tools = tools_with(fakes);
    warp_tools.hyprctl = hyprctl_script;
    auto smart = mynah::inject::make_smart(warp_tools);
    REQUIRE(smart->type_text("в warp"));

    // Warp is keymap-deaf: the clipboard path ran…
    std::ifstream in(fakes.copy_stdin, std::ios::binary);
    std::string copied((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    CHECK(copied.find("в warp") != std::string::npos);
    // …and wtype never typed into it.
    CHECK(read_file(fakes.wtype_stdin).find("в warp") == std::string::npos);

    // Firefox types: wtype ran, not the clipboard.
    std::string hyprctl_firefox = (fakes.dir.path() / "hyprctl-firefox").string();
    {
        std::ofstream out(hyprctl_firefox);
        out << "#!/bin/sh\n";
        out << "cat > /dev/null\n";
        out << "if [ \"$2\" = activewindow ]; then printf '{\"class\": \"firefox\"}'; fi\n";
        out << "exit 0\n";
    }
    ::chmod(hyprctl_firefox.c_str(), 0755);
    Tools firefox_tools = tools_with(fakes);
    firefox_tools.hyprctl = hyprctl_firefox;
    auto smart_firefox = mynah::inject::make_smart(firefox_tools);
    REQUIRE(smart_firefox->type_text("в firefox"));
    CHECK(read_file(fakes.wtype_stdin).find("в firefox") != std::string::npos);
}

TEST_CASE("the wtype chord presses and releases in order") {
    Fakes fakes;
    Tools tools = tools_with(fakes);
    // No wl-paste → nothing to restore; no hyprctl → the chord goes through
    // the wtype fallback. The paste itself needs wl-copy to succeed.
    tools.wl_paste = std::nullopt;
    tools.hyprctl = std::nullopt;

    auto injector = mynah::inject::make_clipboard(tools);
    REQUIRE(injector->type_text("привет"));

    auto argv = recorded(fakes.record);
    std::string chord;
    for (const std::string& line : argv)
        if (line.find("-P") != std::string::npos) chord = line;
    REQUIRE(!chord.empty());
    CHECK(chord.find("-M ctrl -P V -p V -m ctrl") != std::string::npos);
}