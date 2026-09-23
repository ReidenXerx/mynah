#include "setup.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config/config.hpp"
#include "injector.hpp"
#if defined(MYNAH_HAVE_KWIN_TYPER)
#include "kwin_backend.hpp"
#endif
#include "json.hpp"
#include "models/resolve.hpp"
#include "stt/stt.hpp"

namespace mynah::setup {

namespace {

// A check that cannot run must not fail the user: hyprctl absent or not
// Hyprland means "no verdict", same as preflight.py.
bool on_hyprland() { return std::getenv("HYPRLAND_INSTANCE_SIGNATURE") != nullptr; }

Check check_typing() {
    inject::Tools tools = inject::Tools::discover();
#if defined(MYNAH_HAVE_KWIN_TYPER)
    // On KWin the session types through its paste typer (make_auto): wtype
    // and wl-clipboard do not matter there, KWin's permission does.
    if (std::unique_ptr<inject::Injector> kwin = inject::make_kwin()) {
        auto [ok, remedy] = kwin->check();
        if (!ok) return {false, "Typing", remedy.substr(0, remedy.find('\n')),
                         remedy.find('\n') == std::string::npos ? std::string()
                                                                 : remedy.substr(remedy.find('\n') + 1)};
        return {true, "Typing", "KWin: pastes what you say with Shift+Insert, then restores the clipboard",
                ""};
    }
#endif
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
    std::filesystem::path model = models::resolve(config.model, models::search_directories(),
                                                  stt::pick_gpu(config.gpu).has_value());
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

// Where speech will run. Informational unless `gpu` is on and the session
// hides the NVIDIA Vulkan driver (VK_LOADER_DRIVERS_DISABLE — an "iGPU by
// default, dGPU per app" setup): then mynah falls back without a word, and
// this is where the word is.
Check check_gpu() {
    config::Config config = config::load();
    std::optional<stt::GpuChoice> gpu = stt::pick_gpu(config.gpu);
    const char* disabled = std::getenv("VK_LOADER_DRIVERS_DISABLE");
    std::string hidden = disabled ? disabled : "";
    for (char& c : hidden) c = char(std::tolower(static_cast<unsigned char>(c)));
    const bool nvidia_hidden = hidden.find("nvidia") != std::string::npos;
    if (config.gpu && nvidia_hidden && (!gpu || gpu->kind != stt::GpuKind::Discrete))
        return {false, "GPU",
                gpu ? "the integrated GPU (" + gpu->name + "): the NVIDIA driver is hidden"
                    : std::string("the CPU: the NVIDIA driver is hidden"),
                "VK_LOADER_DRIVERS_DISABLE=" + std::string(disabled) +
                    " hides the discrete GPU from mynah.\n"
                    "  Let mynah see it (the dGPU still sleeps between sentences):\n"
                    "  env -u VK_LOADER_DRIVERS_DISABLE mynah\n"
                    "  Or keep the integrated GPU:  mynah set gpu=off"};
    if (!gpu) return {true, "GPU", "none — speech runs on the CPU", ""};
    return {true, "GPU",
            gpu->name + (gpu->kind == stt::GpuKind::Discrete ? " (discrete)" : " (integrated)"),
            ""};
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
    return {check_typing(), check_speech(), check_gpu(), check_microphone(), check_hotkey()};
}

int report(const std::vector<Check>& checks) {
    int failures = 0;
    for (const Check& check : checks) {
        const char* mark = check.ok ? "ok" : "!!";
        std::fprintf(stderr, "  [%s] %s: %s\n", mark, check.title.c_str(),
                     check.detail.c_str());
        // Every line of the hint under the title, not just the first: the
        // hints continue on "\n  " lines, which printed at column 2.
        std::size_t begin = 0;
        while (begin < check.hint.size()) {
            std::size_t end = check.hint.find('\n', begin);
            if (end == std::string::npos) end = check.hint.size();
            std::string line = check.hint.substr(begin, end - begin);
            line.erase(0, std::min<std::size_t>(line.find_first_not_of(' '), 2));
            std::fprintf(stderr, "        %s\n", line.c_str());
            begin = end + 1;
        }
        if (!check.ok) ++failures;
    }
    return failures == 0 ? 0 : 1;
}

} // namespace mynah::setup