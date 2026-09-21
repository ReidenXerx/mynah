// The control socket's implementation (see control_socket.hpp for the
// contract). A faithful port of mynah/control.py — the Omarchy plugin at
// its pinned commit runs against this, so the shapes are load-bearing.

#include "control_socket.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "json.hpp"

namespace mynah::control {

namespace {

std::string protocol_fields(const std::string& version) {
    // v2 (P6): every v1 client ignores fields it does not know.
    return "\"protocol\":2,\"version\":\"" + version + "\"";
}

// How long a slow subscriber may block a publish before we give up on it.
// Level events arrive ~33x a second; a shell that stops reading must
// never be able to stall the audio thread.
constexpr double kSendTimeout = 0.25;

// Level events are the firehose; coalesce them.
constexpr double kLevelMinInterval = 1.0 / 30.0;

std::optional<std::string> env(const char* name) {
    const char* value = std::getenv(name);
    if (value && *value) return std::string(value);
    return std::nullopt;
}

// Create the socket's directory, and refuse to use one we do not own: a
// directory someone else can write to is a directory where someone else
// can replace the socket with their own, and then every `mynah watch` in
// the session is talking to them.
void prepare_dir(const std::string& path) {
    std::filesystem::path directory = std::filesystem::path(path).parent_path();
    std::error_code ec;
    // Tighten ONLY a directory we just created (create_directories makes it
    // 0777 & ~umask, typically 0755). One that already existed is checked
    // as found: chmod-ing it first turned "refuse a world-writable
    // directory" into "quietly accept it" — and by then anyone could have
    // planted their own socket in it. Python's mkdir(mode=0o700) has the
    // same shape: the mode applies on creation, never to what was there.
    if (std::filesystem::create_directories(directory, ec))
        ::chmod(directory.c_str(), 0700);

    struct stat info{};
    if (::stat(directory.c_str(), &info) != 0)
        throw std::runtime_error("cannot stat " + directory.string());
    if (info.st_uid != ::getuid())
        throw std::runtime_error(directory.string() + " is not owned by this user");
    if (info.st_mode & (S_IRWXG | S_IRWXO))
        throw std::runtime_error(directory.string() + " is accessible to other users");
}

bool send_all(int fd, const std::string& blob) {
    struct timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = int(kSendTimeout * 1e6);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::size_t sent = 0;
    while (sent < blob.size()) {
        ssize_t n = ::send(fd, blob.data() + sent, blob.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += std::size_t(n);
    }
    return true;
}

// Read one line (without the newline); empty result means EOF.
std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (::recv(fd, &c, 1, 0) == 1) {
        if (c == '\n') return line;
        line += c;
    }
    return line; // EOF; possibly a partial line
}

std::string state_json(const std::string& state) {
    return "\"state\":\"" + state + "\"";
}

// Fill a sockaddr_un, or throw: a path longer than sun_path (104 bytes on
// macOS, 108 on Linux) used to be strncpy'd in and silently truncated —
// the server then bound a socket at a DIFFERENT path, and every client
// looking at the real one reported "no running mynah" with no hint why.
sockaddr_un unix_address(const std::string& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        throw Error("the control socket path is too long for a unix socket (" +
                    std::to_string(path.size()) + " bytes, the limit is " +
                    std::to_string(sizeof(address.sun_path) - 1) + "): " + path +
                    "\nSet MYNAH_SOCKET to a shorter path.");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

bool someone_home(const std::string& path) {
    int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) return false;
    struct timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000; // 0.2 s
    ::setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    try {
        address = unix_address(path);
    } catch (const Error&) {
        ::close(probe);
        return false;
    }
    bool alive = ::connect(probe, reinterpret_cast<sockaddr*>(&address),
                           sizeof(address)) == 0;
    ::close(probe);
    return alive;
}

} // namespace

std::string socket_path() {
    if (auto override_path = env("MYNAH_SOCKET")) return *override_path;
    std::filesystem::path base;
    if (auto runtime = env("XDG_RUNTIME_DIR"))
        base = std::filesystem::path(*runtime) / "mynah";
    else
        base = std::filesystem::path("/tmp/mynah-" + std::to_string(::getuid()));
    return (base / "control.sock").string();
}

// --- server -------------------------------------------------------------------

Server::Server(Handlers handlers, std::string path, std::string protocol_version)
    : handlers_(std::move(handlers)), path_(std::move(path)),
      version_(std::move(protocol_version)) {
    last_level_at_ = std::chrono::steady_clock::now() - std::chrono::hours(1);
}

Server::~Server() { stop(); }

void Server::start() {
    prepare_dir(path_);
    // A stale socket from a killed process would make bind() fail with
    // EADDRINUSE. Connect first: if someone answers, another mynah owns
    // this socket and we must not steal it.
    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) {
        if (someone_home(path_))
            throw Error("another mynah is already running on " + path_);
        ::unlink(path_.c_str());
    }

    server_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_ < 0) throw Error(std::string("socket() failed: ") + std::strerror(errno));
    sockaddr_un address{};
    try {
        address = unix_address(path_);
    } catch (...) {
        ::close(server_);
        server_ = -1;
        throw;
    }
    if (::bind(server_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(server_);
        server_ = -1;
        throw Error("cannot bind " + path_ + ": " + std::strerror(errno));
    }
    ::chmod(path_.c_str(), 0600);
    ::listen(server_, 8);
    // Non-blocking, so a connection that vanishes between poll() and
    // accept() costs one EAGAIN instead of a thread stuck in accept().
    ::fcntl(server_, F_SETFL, ::fcntl(server_, F_GETFL) | O_NONBLOCK);
    if (::pipe(wake_pipe_) != 0) {
        ::close(server_);
        server_ = -1;
        throw Error(std::string("pipe() failed: ") + std::strerror(errno));
    }
    for (int fd : {server_, wake_pipe_[0], wake_pipe_[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    stopping_.store(false);
    accept_thread_ = std::thread(
        [this, listening = server_, wake = wake_pipe_[0]] { accept_loop(listening, wake); });
}

void Server::spawn(std::function<void()> body) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread thread([body = std::move(body), done] {
        body();
        done->store(true, std::memory_order_release);
    });
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers_.push_back(Worker{std::move(thread), std::move(done)});
}

void Server::reap_finished() {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    std::erase_if(workers_, [](Worker& worker) {
        if (!worker.done->load(std::memory_order_acquire)) return false;
        if (worker.thread.joinable()) worker.thread.join(); // finished: instant
        return true;
    });
}

void Server::stop() {
    if (accept_thread_.joinable()) {
        stopping_.store(true);
        // Wake the accept thread through its pipe; the listening fd is
        // closed only once that thread has been joined, so its number
        // cannot be handed to a new socket while the thread might use it.
        char byte = 0;
        (void)!::write(wake_pipe_[1], &byte, 1);
        accept_thread_.join();
        ::close(server_);
        server_ = -1;
        ::close(wake_pipe_[0]);
        ::close(wake_pipe_[1]);
        wake_pipe_[0] = wake_pipe_[1] = -1;
    }
    // Wake every serve thread with shutdown(), not close(): on Linux,
    // closing an fd does NOT interrupt a recv() another thread is blocked
    // in — stop() would then wait forever on any client that connected and
    // said nothing — and a closed fd number can be reused under a thread
    // still using it. shutdown() makes that recv() return 0 on every
    // platform; the serve thread then drops (and closes) its own fd.
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (int fd : connections_) ::shutdown(fd, SHUT_RDWR);
    }
    // Join every worker: connection threads, and commands still being
    // carried out — a stop that returned while a handler ran would leave it
    // calling into whatever the caller destroys next. Taken out of the list
    // and joined outside the lock, round after round, because a connection
    // thread can still spawn a handler while it winds down.
    for (;;) {
        std::vector<Worker> batch;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            batch.swap(workers_);
        }
        if (batch.empty()) break;
        for (Worker& worker : batch)
            if (worker.thread.joinable()) worker.thread.join();
    }
    // Every thread is gone. What is still tracked has no serve thread left
    // to close it — the subscribers — so close those here: exactly one
    // close per fd, by whoever removes it from connections_.
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (int fd : connections_) ::close(fd);
        connections_.clear();
        subscribers_.clear();
    }
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

void Server::track_connection(int fd) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.push_back(fd);
}

void Server::drop_connection(int fd) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    if (auto it = std::find(connections_.begin(), connections_.end(), fd);
        it != connections_.end()) {
        connections_.erase(it);
        ::close(fd);
    }
    if (auto it = std::find(subscribers_.begin(), subscribers_.end(), fd);
        it != subscribers_.end())
        subscribers_.erase(it);
}

void Server::accept_loop(int listening, int wake) {
    while (!stopping_.load()) {
        pollfd fds[2] = {{listening, POLLIN, 0}, {wake, POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (stopping_.load() || (fds[1].revents & POLLIN)) break;
        if (!(fds[0].revents & POLLIN)) continue;
        int connection = ::accept(listening, nullptr, nullptr);
        if (connection < 0) continue; // EAGAIN: the client left before we got to it
        ::fcntl(connection, F_SETFD, FD_CLOEXEC);
        // Blocking, explicitly: BSD and macOS hand accepted sockets the
        // listening socket's O_NONBLOCK (Linux does not), and serve()'s
        // recv() would then return EAGAIN at once and drop a client that
        // simply had not written yet.
        ::fcntl(connection, F_SETFL, ::fcntl(connection, F_GETFL) & ~O_NONBLOCK);
        track_connection(connection);
        reap_finished(); // every connection is a good moment to tidy up
        spawn([this, connection] { serve(connection); });
    }
}

void Server::serve(int connection) {
    std::string buffer;
    while (!stopping_.load()) {
        char c = 0;
        ssize_t n = ::recv(connection, &c, 1, 0);
        if (n <= 0) break; // the client went away
        if (c != '\n') {
            buffer += c;
            continue;
        }
        std::string line = std::exchange(buffer, {});
        if (line.empty()) continue; // blank line: skipped, not EOF
        auto [reply, subscribe] = handle(line);
        if (subscribe) {
            // Send the ack and join the subscribers under ONE hold of the
            // lock publish() copies the list under. Registering after the
            // ack left a window in which an event was published to nobody
            // — a state change lost right after `mynah watch` connected —
            // and registering before it would let an event overtake the ack
            // the plugin reads first. Under the lock, neither can happen.
            std::lock_guard<std::mutex> lock(connections_mutex_);
            if (!send_all(connection, reply + "\n")) break;
            subscribers_.push_back(connection);
            // The connection now belongs to the publish path; stop reading
            // from this thread. Its fd is still tracked in connections_,
            // so stop() closes it.
            return;
        }
        if (!send_all(connection, reply + "\n")) break;
    }
    drop_connection(connection);
}

std::pair<std::string, bool> Server::handle(const std::string& line) {
    // Be forgiving: a bare word is a command too, so
    // `echo toggle | nc ...` works from a script without quoting JSON.
    std::string command;
    try {
        json::Value message = json::parse(line);
        if (!message.is_object())
            return {"{\"ok\":false,\"error\":\"expected an object\"}", false};
        const json::Value* cmd = message.find("cmd");
        if (cmd != nullptr && cmd->tag == json::Value::Tag::String)
            command = cmd->text;
        else if (cmd != nullptr)
            command = "null"; // Python's str(message.get("cmd")) shape
    } catch (const std::exception&) {
        command = line;
    }
    // Lowercase + trim, like Python's str(...).strip().lower().
    std::size_t begin = command.find_first_not_of(" \t\r");
    std::size_t end = command.find_last_not_of(" \t\r");
    command = begin == std::string::npos
                  ? ""
                  : command.substr(begin, end - begin + 1);
    for (char& c : command) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (command == "subscribe")
        return {"{\"ok\":true," + state_json(handlers_.state()) + "," +
                    protocol_fields(version_) + "}",
                true};
    if (command == "status")
        return {"{\"ok\":true," + state_json(handlers_.state()) +
                    ",\"pid\":" + std::to_string(::getpid()) + "," +
                    protocol_fields(version_) + "}",
                false};

    std::function<void()>* handler = nullptr;
    if (command == "toggle") handler = &handlers_.toggle;
    else if (command == "start") handler = &handlers_.start;
    else if (command == "stop") handler = &handlers_.stop;
    else if (command == "quit") handler = &handlers_.quit;
    if (handler == nullptr)
        return {"{\"ok\":false,\"error\":\"unknown command '" + json::escape(command) +
                    "'\"}",
                false};

    // Acknowledge now, carry out afterwards: what came of the command
    // arrives as state events, which is where a caller should be reading
    // the truth from anyway. A failing handler reaches subscribers as an
    // error event rather than as a broken connection.
    std::function<void()> carry_out = *handler;
    spawn([this, command, carry_out] {
        try {
            carry_out();
        } catch (const std::exception& e) {
            publish("{\"event\":\"error\",\"command\":\"" + json::escape(command) +
                    "\",\"error\":\"" + json::escape(e.what()) + "\"}");
        }
    });
    return {"{\"ok\":true," + state_json(handlers_.state()) + "," +
                protocol_fields(version_) + "}",
            false};
}

void Server::publish(const std::string& event_json) {
    std::vector<int> subscribers;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        if (subscribers_.empty()) return;
        subscribers = subscribers_;
    }
    std::string blob = event_json + "\n";
    for (int fd : subscribers) {
        if (!send_all(fd, blob)) {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            if (auto it = std::find(connections_.begin(), connections_.end(), fd);
                it != connections_.end()) {
                connections_.erase(it);
                ::close(fd);
            }
            if (auto it = std::find(subscribers_.begin(), subscribers_.end(), fd);
                it != subscribers_.end())
                subscribers_.erase(it);
        }
    }
}

void Server::publish_level(double level, const std::string& bands_json) {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last_level_at_).count() < kLevelMinInterval)
        return;
    last_level_at_ = now;
    char level_text[32];
    std::snprintf(level_text, sizeof(level_text), "%.4f", level);
    std::string event = "{\"event\":\"level\",\"level\":" + std::string(level_text);
    if (!bands_json.empty()) event += ",\"bands\":[" + bands_json + "]";
    event += "}";
    publish(event);
}

// --- client ---------------------------------------------------------------------

int connect(const std::string& path, double timeout_seconds) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw Error("cannot create a socket");
    struct timeval timeout{};
    timeout.tv_sec = time_t(timeout_seconds);
    timeout.tv_usec = suseconds_t((timeout_seconds - double(timeout.tv_sec)) * 1e6);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    try {
        address = unix_address(path);
    } catch (...) {
        ::close(fd);
        throw;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        throw Error("no running mynah on " + path + " (" + std::strerror(errno) +
                    ").\nStart one with:  mynah        (or: systemctl --user start mynah)");
    }
    return fd;
}

std::string command(const std::string& cmd, double timeout_seconds) {
    std::string path = socket_path();
    int fd = connect(path, timeout_seconds);
    std::string reply;
    try {
        std::string request = "{\"cmd\":" + json::quoted(cmd) + "}\n";
        if (!send_all(fd, request))
            throw Error("lost the connection to mynah");
        reply = read_line(fd);
        if (reply.empty())
            throw Error("mynah closed the connection without replying");
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    try {
        json::parse(reply); // must be JSON, or the caller gets a clear error
    } catch (const std::exception&) {
        throw Error("mynah answered with something that was not JSON");
    }
    return reply;
}

} // namespace mynah::control