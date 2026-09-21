#include "setup.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config/config.hpp"
#include "injector.hpp"
#include "json.hpp"
#include "models/resolve.hpp"

namespace mynah::setup {

namespace {

// A check that cannot run must not fail the user: hyprctl absent or not
// Hyprland means "no verdict", same as preflight.py.
bool on_hyprland() { return std::getenv("HYPRLAND_INSTANCE_SIGNATURE") != nullptr; }

Check check_typing() {
    inject::Tools tools = inject::Tools::discover();
    auto [ok, remedy] = inject::make_smart(tools)->check();
    if (!ok) return {false, "Typing", remedy.substr(0, remedy.find('\n')),
                     remedy.substr(remedy.find('\n') + 1)};
    if (tools.wl_copy && tools.wl_paste)
        return {true, "Typing", "wtype, and the clipboard for apps that ignore it", ""};
    return {true, "Typing", "wtype is installed — but nothing to paste with",
            "Some apps (Warp, and anything else that ignores a virtual\n"
            "  keyboard) turn typed text into digits. Install wl-clipboard\n"
            "  and mynah pastes into those instead:\n"
            "  sudo pacman -S wl-clipboard"};
}

Check check_speech() {
    // whisper.cpp is compiled in; the question is whether a model is.
    config::Config config = config::load();
    std::filesystem::path model = models::resolve(config.model, models::search_directories());
    if (!model.empty())
        return {true, "Speech", "whisper.cpp with " + model.filename().string(), ""};
    std::string wanted = config.model.empty() ? std::string("small") : config.model;
    return {false, "Speech", "No whisper model named '" + wanted + "'",
            "Download it:\n"
            "  mynah models download " +
                wanted +
                "\n"
                "  Or point at one you have:  mynah set model=/path/to/ggml-small.bin"};
}

Check check_microphone() {
    // Read-only: can we see the session's PipeWire? The socket is named
    // after the runtime dir; its absence is the daemon-not-running case.
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string pipewire_socket =
        runtime ? std::string(runtime) + "/pipewire-0" : std::string("/run/user/0/pipewire-0");
    std::error_code ec;
    if (std::filesystem::exists(pipewire_socket, ec))
        return {true, "Microphone", "PipeWire is running", ""};
    return {false, "Microphone", "PipeWire is not reachable",
            "The capture stream needs the session's PipeWire daemon.\n"
            "  On a running desktop it is always there; over bare ssh it is not.\n"
            "  Check:  systemctl --user status pipewire"};
}

Check check_hotkey() {
    if (!on_hyprland())
        return {true, "Hotkey", "Bound in your compositor (mynah cannot read it from here)",
                "Bind a key to:  mynah toggle"};
    inject::Tools tools = inject::Tools::discover();
    if (!tools.hyprctl) return {true, "Hotkey", "Bound in your compositor", "Bind a key to:  mynah toggle"};
    // hyprctl -j binds -> look for "mynah" in dispatcher/arg/description.
    // A binding made in Lua — the Omarchy plugin's way — reports its
    // dispatcher as "__lua" and its argument as a table index, so the
    // command is not in the binding at all; its description is.
    std::string output;
    int pipe_fd[2];
    if (::pipe(pipe_fd) == 0) {
        pid_t pid = ::fork();
        if (pid == 0) {
            ::dup2(pipe_fd[1], STDOUT_FILENO);
            ::close(pipe_fd[0]);
            ::close(pipe_fd[1]);
            ::execl(tools.hyprctl->c_str(), tools.hyprctl->c_str(), "-j", "binds",
                    static_cast<char*>(nullptr));
            _exit(127);
        }
        ::close(pipe_fd[1]);
        char buffer[4096];
        ssize_t n;
        while ((n = ::read(pipe_fd[0], buffer, sizeof(buffer))) > 0)
            output.append(buffer, std::size_t(n));
        ::close(pipe_fd[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) output.clear();
    }
    try {
        json::Value binds = json::parse(output);
        if (!binds.is_array()) return {true, "Hotkey", "Bound in your compositor",
                                       "Bind a key to:  mynah toggle"};
        for (const json::Value& bind : binds.array) {
            std::string haystack;
            for (const char* field : {"dispatcher", "arg", "description"})
                if (const json::Value* value = bind.find(field))
                    if (value->tag == json::Value::Tag::String) haystack += " " + value->text;
            for (char& c : haystack) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (haystack.find("mynah") != std::string::npos)
                return {true, "Hotkey",
                        "A Hyprland binding runs mynah",
                        ""};
        }
        return {false, "Hotkey", "No Hyprland binding runs mynah",
                "Add one, then reload:\n"
                "  bind = SUPER, D, exec, mynah toggle\n"
                "  The Omarchy plugin binds it for you: "
                "omarchy plugin add https://github.com/ReidenXerx/omarchy-mynah.git --enable"};
    } catch (const std::exception&) {
        return {true, "Hotkey", "Bound in your compositor (could not read the bindings)",
                "Bind a key to:  mynah toggle"};
    }
}

} // namespace

std::vector<Check> run_checks() {
    return {check_typing(), check_speech(), check_microphone(), check_hotkey()};
}

int report(const std::vector<Check>& checks) {
    int failures = 0;
    for (const Check& check : checks) {
        const char* mark = check.ok ? "ok" : "!!";
        std::fprintf(stderr, "  [%s] %s: %s\n", mark, check.title.c_str(),
                     check.detail.c_str());
        if (!check.ok) {
            if (!check.hint.empty())
                std::fprintf(stderr, "        %s\n", check.hint.c_str());
            ++failures;
        } else if (!check.hint.empty()) {
            std::fprintf(stderr, "        %s\n", check.hint.c_str());
        }
    }
    return failures == 0 ? 0 : 1;
}

} // namespace mynah::setup