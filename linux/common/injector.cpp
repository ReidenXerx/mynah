#include "injector.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/wait.h>

#include "json.hpp"
#if defined(MYNAH_HAVE_KWIN_TYPER)
#include "kwin_backend.hpp"
#endif

namespace mynah::inject {

namespace {

// A wedged `wtype` must not wedge dictation. Typing a long utterance is
// still well under a second.
constexpr double kTypeTimeout = 10.0;

// How long our text stays on the clipboard before putting back what was
// there: the paste is synchronous from the compositor's side, but the app
// reads the selection asynchronously, and reading a clipboard that has
// already been replaced yields nothing.
constexpr double kRestoreAfter = 0.8;

// Terminals paste with Ctrl+Shift+V, because Ctrl+V is a control
// character to the program inside them. Matched as substrings of the
// lowercased window class.
const char* kTerminalClasses[] = {
    "warp",      "foot",         "kitty",       "alacritty", "wezterm",
    "ghostty",   "konsole",      "gnome-terminal", "xterm",  "urxvt",
    "terminator", "tilix",       "blackbox",    "org.wezfurlong",
    "com.mitchellh.ghostty",     "st-256color",
};

// Each entry is an app observed turning typed text into digits — what a
// virtual keymap being ignored looks like. Everything else types, because
// typing needs no clipboard and no shortcut.
const char* kKeymapDeaf[] = {"warp"};

struct RunResult {
    bool ok = false;
    std::string output;
};

// fork/exec with the text on stdin (never in argv) and a timeout that is
// actually enforced. Three things this has to get right, each learned the
// hard way:
//
//   1. The child must hold NOTHING but its own stdin/stdout/stderr. A
//      child that inherits the write end of its own stdin pipe never sees
//      EOF — and `wtype -` and `wl-copy` both read stdin to EOF, so the
//      first version hung on the first utterance. Every descriptor we make
//      is close-on-exec, and the child closes everything above stderr
//      before exec: the engine's sockets and model files must not ride
//      along into wl-copy's long-lived clipboard daemon either.
//   2. A child that exits before reading its stdin (wtype on a compositor
//      without the virtual-keyboard protocol) must not kill us. Stdin is a
//      socketpair so the text can be sent with MSG_NOSIGNAL: an early exit
//      is an EPIPE we can see, not a SIGPIPE that ends the daemon.
//   3. The deadline has to hold whatever the child does. Nothing here
//      blocks: stdin is fed and output drained from one poll() loop, so a
//      wedged child costs at most `timeout_seconds` and is then killed.
//
// Output is captured on request; wl-copy's is not (its daemon inherits the
// output and never exits, so waiting for EOF on it would wait forever).
void set_cloexec(int fd) { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }

void set_nonblocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK); }

// Close every descriptor above stderr in the child. Called between fork and
// exec, so only async-signal-safe calls — `max_fd` is computed before fork.
void close_inherited(int max_fd) {
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3U, ~0U, 0U) == 0) return;
#endif
    for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
}

RunResult run(const std::optional<std::string>& binary, const std::vector<std::string>& argv,
              const std::string& stdin_text, double timeout_seconds, bool capture_output) {
    if (!binary) return {};

    int in_pair[2] = {-1, -1};   // [0] parent writes, [1] child's stdin
    int out_pipe[2] = {-1, -1};  // [0] parent reads, [1] child's stdout/stderr
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, in_pair) != 0) return {};
    if (capture_output && ::pipe(out_pipe) != 0) {
        ::close(in_pair[0]);
        ::close(in_pair[1]);
        return {};
    }
    for (int fd : {in_pair[0], in_pair[1], out_pipe[0], out_pipe[1]})
        if (fd >= 0) set_cloexec(fd);

    long open_max = ::sysconf(_SC_OPEN_MAX);
    const int max_fd = open_max > 0 && open_max < 65536 ? int(open_max) : 65536;

    pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {in_pair[0], in_pair[1], out_pipe[0], out_pipe[1]})
            if (fd >= 0) ::close(fd);
        return {};
    }
    if (pid == 0) {
        // Child. dup2 clears close-on-exec on the copy it makes, so these
        // three survive exec and nothing else we opened does.
        ::dup2(in_pair[1], STDIN_FILENO);
        int sink = capture_output ? out_pipe[1] : ::open("/dev/null", O_WRONLY);
        if (sink >= 0) {
            ::dup2(sink, STDOUT_FILENO);
            ::dup2(sink, STDERR_FILENO);
        }
        close_inherited(max_fd);
        std::vector<char*> argv_c;
        argv_c.reserve(argv.size() + 1);
        for (const std::string& arg : argv) argv_c.push_back(const_cast<char*>(arg.c_str()));
        argv_c.push_back(nullptr);
        ::execv(binary->c_str(), argv_c.data());
        _exit(127); // exec failed: the binary exists but cannot be run
    }

    // Parent: drop the child's ends, so EOF reaches whoever reads.
    ::close(in_pair[1]);
    if (capture_output) ::close(out_pipe[1]);

    int in_fd = in_pair[0];
    int out_fd = capture_output ? out_pipe[0] : -1;
    set_nonblocking(in_fd);
    if (out_fd >= 0) set_nonblocking(out_fd);

    std::size_t written = 0;
    auto close_stdin = [&] {
        if (in_fd >= 0) {
            ::close(in_fd); // EOF for the child
            in_fd = -1;
        }
    };
    if (stdin_text.empty()) close_stdin();

    std::string output;
    auto drain = [&] {
        if (out_fd < 0) return;
        char buffer[4096];
        for (;;) {
            ssize_t n = ::read(out_fd, buffer, sizeof(buffer));
            if (n > 0) {
                output.append(buffer, std::size_t(n));
                continue;
            }
            if (n == 0) { // EOF: every writer is gone
                ::close(out_fd);
                out_fd = -1;
            }
            return; // EAGAIN: nothing more right now
        }
    };

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
    for (;;) {
        // Feed stdin as far as the child will take it.
        if (in_fd >= 0) {
            ssize_t n = ::send(in_fd, stdin_text.data() + written, stdin_text.size() - written,
                               MSG_NOSIGNAL);
            if (n > 0) written += std::size_t(n);
            if (written == stdin_text.size() ||
                (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                close_stdin(); // all sent — or the child stopped reading (EPIPE)
        }
        drain();

        int status = 0;
        pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            close_stdin();
            drain(); // whatever it wrote before exiting
            if (out_fd >= 0) ::close(out_fd); // a grandchild may hold it open
            RunResult result;
            result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            result.output = std::move(output);
            return result;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        // Sleep until something happens or 10 ms pass — whichever is first.
        // The short cap is what notices the child exiting (poll does not).
        pollfd fds[2];
        nfds_t count = 0;
        if (in_fd >= 0) fds[count++] = {in_fd, POLLOUT, 0};
        if (out_fd >= 0) fds[count++] = {out_fd, POLLIN, 0};
        int wait_ms = int(std::min<long long>(
            10, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1));
        if (count > 0) ::poll(fds, count, wait_ms);
        else std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }

    // Past the deadline: a wedged tool must not wedge dictation.
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    close_stdin();
    if (out_fd >= 0) ::close(out_fd);
    return {};
}

std::optional<std::string> which(const char* name) {
    std::string view = name;
    if (view.find('/') != std::string::npos) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(view, ec)) return view;
        return std::nullopt;
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;
    std::string directories = path_env;
    std::size_t begin = 0;
    while (begin <= directories.size()) {
        std::size_t end = directories.find(':', begin);
        std::string directory = directories.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        std::error_code ec;
        std::filesystem::path candidate = std::filesystem::path(directory) / name;
        if (!directory.empty() && access(candidate.c_str(), X_OK) == 0)
            return candidate.string();
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return std::nullopt;
}

// --- wtype ---------------------------------------------------------------------

class WtypeInjector final : public Injector {
public:
    explicit WtypeInjector(Tools tools) : tools_(tools) {}

    bool type_text(const std::string& text) override {
        if (text.empty()) return true;
        if (!tools_.wtype) return false;
        // Text on stdin, never in argv (see the header).
        return run(tools_.wtype, {*tools_.wtype, "-"}, text, kTypeTimeout,
                   /*capture_output=*/true)
            .ok;
    }

    // Press a shortcut, e.g. ("CTRL SHIFT", "V") — modifiers are wtype's
    // own names, pressed before the key and released after it, in order.
    bool send_chord(const std::string& mods, const std::string& key) const {
        if (!tools_.wtype) return false;
        std::vector<std::string> argv{*tools_.wtype};
        std::vector<std::string> names;
        std::size_t begin = 0;
        while (begin <= mods.size()) {
            std::size_t end = mods.find(' ', begin);
            std::string name = mods.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!name.empty()) names.push_back(name);
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        for (const std::string& name : names) {
            argv.push_back("-M");
            argv.push_back(name);
        }
        argv.push_back("-P");
        argv.push_back(key);
        argv.push_back("-p");
        argv.push_back(key);
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            argv.push_back("-m");
            argv.push_back(*it);
        }
        return run(tools_.wtype, argv, "", kTypeTimeout, /*capture_output=*/true).ok;
    }

    std::pair<bool, std::string> check() const override {
        if (!is_wayland()) {
            const char* session = std::getenv("XDG_SESSION_TYPE");
            return {false,
                    std::string("Not a Wayland session (XDG_SESSION_TYPE=") +
                        (session ? session : "unset") + ").\n" +
                        "mynah types through the Wayland virtual-keyboard protocol and "
                        "does not ship an X11 path — see docs/LINUX-APP.md."};
        }
        if (!tools_.wtype) {
            return {false,
                    "wtype is not installed — mynah cannot type what it hears.\n"
                    "  Arch:   sudo pacman -S wtype\n"
                    "  Debian: sudo apt install wtype"};
        }
        return {true, ""};
    }

private:
    Tools tools_;
};

// --- clipboard -------------------------------------------------------------------

class ClipboardInjector final : public Injector {
public:
    explicit ClipboardInjector(Tools tools)
        : tools_(tools), fallback_(std::make_unique<WtypeInjector>(tools)) {}

    // A restore still pending runs before this returns (at most
    // kRestoreAfter): quitting right after dictating must not leave the
    // dictated text on the clipboard in place of the user's.
    ~ClipboardInjector() override {
        {
            std::lock_guard<std::mutex> lock(restore_mutex_);
            stopping_ = true;
        }
        restore_wake_.notify_all();
        if (restorer_.joinable()) restorer_.join();
    }

    bool type_text(const std::string& text) override {
        if (text.empty()) return true;
        // None means "do not try to restore": either nothing is there, or
        // it is an image or a file list, which this cannot carry — an
        // image on the clipboard is left alone.
        std::optional<std::string> saved = read_clipboard();
        if (!write_clipboard(text)) return fallback_->type_text(text);
        auto [mods, key] = paste_chord(focused_class(tools_.hyprctl));
        if (!send_chord(mods, key)) {
            restore_later(saved);
            return false; // the text is on the clipboard; nothing else to do
        }
        restore_later(saved);
        return true;
    }

    std::pair<bool, std::string> check() const override {
        if (!is_wayland()) {
            return {false,
                    "Not a Wayland session. mynah does not ship an X11 path — "
                    "see docs/LINUX-APP.md."};
        }
        std::string missing;
        if (!tools_.wl_copy) missing = missing.empty() ? "wl-copy" : missing + " wl-copy";
        if (!tools_.wl_paste) missing = missing.empty() ? "wl-paste" : missing + " wl-paste";
        if (!missing.empty()) {
            return {false,
                    missing + " not installed — the clipboard injector needs them.\n"
                    "  Arch:   sudo pacman -S wl-clipboard\n"
                    "  Debian: sudo apt install wl-clipboard"};
        }
        return {true, ""};
    }

private:
    std::optional<std::string> read_clipboard() const {
        if (!tools_.wl_paste) return std::nullopt;
        // Types first: an image or a file list has no text/plain, and
        // restoring over it would destroy the user's clipboard content.
        RunResult types = run(tools_.wl_paste, {*tools_.wl_paste, "--list-types"}, "",
                              2.0, true);
        if (!types.ok || types.output.find("text/plain") == std::string::npos)
            return std::nullopt;
        RunResult text = run(tools_.wl_paste,
                             {*tools_.wl_paste, "--no-newline", "--type", "text/plain"},
                             "", 2.0, true);
        if (!text.ok) return std::nullopt;
        return text.output;
    }

    bool write_clipboard(const std::string& text) const {
        if (!tools_.wl_copy) return false;
        // wl-copy forks a daemon that serves the selection until something
        // replaces it, and that daemon inherits the pipes — so the output
        // goes to /dev/null and only the parent is waited for.
        return run(tools_.wl_copy,
                   {*tools_.wl_copy, "--type", "text/plain", "--"}, text, 5.0,
                   /*capture_output=*/false)
            .ok;
    }

    bool send_chord(const std::string& mods, const std::string& key) const {
        // Through Hyprland rather than through a virtual keyboard: the
        // whole reason we are here is an app that ignores virtual
        // keyboards. Elsewhere, the virtual keyboard is all there is — and
        // it works in every app that does not have this problem.
        const char* hyprland = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if (tools_.hyprctl && hyprland) {
            std::string lua = "hl.dsp.send_shortcut({ mods = \"" + mods + "\", key = \"" +
                               key + "\", window = \"activewindow\" })";
            RunResult done =
                run(tools_.hyprctl, {*tools_.hyprctl, "dispatch", lua}, "", 3.0, true);
            std::string text = done.output;
            for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (done.ok && text.find("error") == std::string::npos) return true;
        }
        return fallback_->send_chord(mods, key);
    }

    void restore_later(std::optional<std::string> saved) const {
        std::lock_guard<std::mutex> lock(restore_mutex_);
        // One restore, however many pastes: a second utterance inside the
        // window reads OUR text off the clipboard, so the first saved
        // content is the user's and is kept; each paste only moves the
        // deadline, so no restore lands while an app is still pasting.
        if (saved && !pending_) pending_ = std::move(saved);
        if (!pending_) return;
        restore_at_ = std::chrono::steady_clock::now() +
                      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<double>(kRestoreAfter));
        if (!restorer_.joinable()) restorer_ = std::thread([this] { restore_loop(); });
        restore_wake_.notify_all();
    }

    // The one restorer thread, joined by the destructor — never detached,
    // so it cannot outlive the injector it writes through.
    void restore_loop() const {
        std::unique_lock<std::mutex> lock(restore_mutex_);
        for (;;) {
            if (!pending_) {
                if (stopping_) return;
                restore_wake_.wait(lock);
                continue;
            }
            if (std::chrono::steady_clock::now() < restore_at_) {
                restore_wake_.wait_until(lock, restore_at_);
                continue; // a newer paste may have moved the deadline
            }
            std::string text = std::move(*pending_);
            pending_.reset();
            lock.unlock();
            write_clipboard(text);
            lock.lock();
        }
    }

    Tools tools_;
    std::unique_ptr<WtypeInjector> fallback_;

    mutable std::mutex restore_mutex_;
    mutable std::condition_variable restore_wake_;
    mutable std::thread restorer_;
    mutable std::optional<std::string> pending_;
    mutable std::chrono::steady_clock::time_point restore_at_;
    mutable bool stopping_ = false;
};

// --- smart ------------------------------------------------------------------------

class SmartInjector final : public Injector {
public:
    explicit SmartInjector(Tools tools)
        : tools_(tools), typing_(make_wtype(tools)), pasting_(make_clipboard(tools)),
          last_looked_at_(std::chrono::steady_clock::now() - std::chrono::hours(1)) {}

    bool type_text(const std::string& text) override {
        std::string window_class = class_now();
        for (const char* deaf : kKeymapDeaf)
            if (window_class.find(deaf) != std::string::npos)
                return pasting_->type_text(text);
        return typing_->type_text(text);
    }

    std::pair<bool, std::string> check() const override {
        // Typing is the normal path, so its requirements are the ones to report.
        return typing_->check();
    }

private:
    // Cache the class between utterances of one burst: focus rarely moves
    // mid-sentence, and this is a subprocess.
    std::string class_now() {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_looked_at_).count() < 0.5)
            return last_class_;
        last_class_ = focused_class(tools_.hyprctl);
        last_looked_at_ = now;
        return last_class_;
    }

    Tools tools_;
    std::unique_ptr<Injector> typing_;
    std::unique_ptr<Injector> pasting_;
    std::string last_class_;
    std::chrono::steady_clock::time_point last_looked_at_;
};

} // namespace

Tools Tools::discover() {
    Tools tools;
    if (const char* override_path = std::getenv("MYNAH_WTYPE")) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(override_path, ec))
            tools.wtype = override_path;
    } else {
        tools.wtype = which("wtype");
    }
    tools.wl_copy = which("wl-copy");
    tools.wl_paste = which("wl-paste");
    tools.hyprctl = which("hyprctl");
    return tools;
}

bool is_wayland() {
    const char* display = std::getenv("WAYLAND_DISPLAY");
    const char* session = std::getenv("XDG_SESSION_TYPE");
    return (display && *display) || (session && std::string(session) == "wayland");
}

std::pair<std::string, std::string> paste_chord(const std::string& window_class) {
    for (const char* name : kTerminalClasses)
        if (window_class.find(name) != std::string::npos)
            return {"CTRL SHIFT", "V"};
    return {"CTRL", "V"};
}

std::string focused_class(const std::optional<std::string>& hyprctl) {
    if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return "";
    if (!hyprctl) return "";
    RunResult done = run(hyprctl, {*hyprctl, "-j", "activewindow"}, "", 2.0, true);
    if (!done.ok) return "";
    try {
        json::Value window = json::parse(done.output);
        if (const json::Value* class_value = window.find("class"))
            if (class_value->tag == json::Value::Tag::String) {
                std::string text = class_value->text;
                for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return text;
            }
    } catch (const std::exception&) {
    }
    return "";
}

std::unique_ptr<Injector> make_wtype(Tools tools) {
    return std::make_unique<WtypeInjector>(std::move(tools));
}

std::unique_ptr<Injector> make_clipboard(Tools tools) {
    return std::make_unique<ClipboardInjector>(std::move(tools));
}

std::unique_ptr<Injector> make_smart(Tools tools) {
    return std::make_unique<SmartInjector>(std::move(tools));
}

std::unique_ptr<Injector> make_auto(Tools tools) {
#if defined(MYNAH_HAVE_KWIN_TYPER)
    if (std::unique_ptr<Injector> kwin = make_kwin()) return kwin;
#endif
    return make_smart(std::move(tools));
}

} // namespace mynah::inject