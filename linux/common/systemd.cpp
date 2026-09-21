#include "systemd.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace mynah::systemd {

namespace {

bool have_systemctl() {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;
    std::string directories = path_env;
    std::size_t begin = 0;
    while (begin <= directories.size()) {
        std::size_t end = directories.find(':', begin);
        std::string directory = directories.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        std::string candidate = directory + "/systemctl";
        if (!directory.empty() && ::access(candidate.c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

// systemctl --user <args...>; returns (exit_code, combined stdout).
std::pair<int, std::string> systemctl(const std::vector<std::string>& args) {
    std::vector<std::string> argv{"systemctl", "--user"};
    argv.insert(argv.end(), args.begin(), args.end());

    int pipe_fd[2];
    if (::pipe(pipe_fd) != 0) return {1, ""};
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        return {1, ""};
    }
    if (pid == 0) {
        ::dup2(pipe_fd[1], STDOUT_FILENO);
        ::dup2(pipe_fd[1], STDERR_FILENO);
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        std::vector<char*> argv_c;
        for (const std::string& arg : argv) argv_c.push_back(const_cast<char*>(arg.c_str()));
        argv_c.push_back(nullptr);
        ::execvp("systemctl", argv_c.data());
        _exit(127);
    }
    ::close(pipe_fd[1]);
    std::string output;
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(pipe_fd[0], buffer, sizeof(buffer))) > 0) output.append(buffer, std::size_t(n));
    ::close(pipe_fd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 1, output};
}

std::string trim(std::string text) {
    std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

} // namespace

int install() {
    if (!have_systemctl()) {
        std::fprintf(stderr,
                     "systemd is not available here, so there is no login service to "
                     "install.\nStart mynah from your compositor's autostart instead:  "
                     "mynah\n");
        return 1;
    }
    auto [reload_code, reload_output] = systemctl({"daemon-reload"});
    if (reload_code != 0) {
        std::fprintf(stderr, "systemctl daemon-reload failed: %s\n",
                     trim(reload_output).c_str());
        return 1;
    }
    auto [enable_code, enable_output] = systemctl({"enable", "--now", kUnit});
    if (enable_code != 0) {
        std::fprintf(stderr, "Could not enable %s: %s\n", kUnit,
                     trim(enable_output).c_str());
        return 1;
    }
    std::fprintf(stderr,
                 "%s is enabled and running.\n"
                 "  Bind a key in your compositor to:  mynah toggle\n"
                 "  Logs:    journalctl --user -u %s -f\n"
                 "  Status:  mynah service status\n",
                 kUnit, kUnit);
    return 0;
}

int uninstall() {
    if (have_systemctl()) systemctl({"disable", "--now", kUnit});
    if (have_systemctl()) systemctl({"daemon-reload"});
    std::fprintf(stderr, "Stopped and disabled %s.\n", kUnit);
    return 0;
}

int status() {
    if (!have_systemctl()) {
        std::fprintf(stderr, "systemctl is not available.\n");
        return 1;
    }
    auto [enabled_code, enabled] = systemctl({"is-enabled", kUnit});
    auto [active_code, active] = systemctl({"is-active", kUnit});
    std::string enabled_text = trim(enabled);
    std::string active_text = trim(active);
    if (enabled_text.empty()) enabled_text = "unknown";
    if (active_text.empty()) active_text = "unknown";
    std::fprintf(stderr, "%s: %s, %s\n  /usr/lib/systemd/user/%s\n", kUnit, active_text.c_str(),
                 enabled_text.c_str(), kUnit);
    if (active_text != "active") {
        std::fprintf(stderr, "  Why:  journalctl --user -u %s -n 20 --no-pager\n", kUnit);
        return 1;
    }
    return 0;
}

} // namespace mynah::systemd