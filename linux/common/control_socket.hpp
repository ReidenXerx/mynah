// mynah::control — the control socket: how anything outside the process
// drives dictation (Phase 3, the port of mynah/control.py).
//
// On Wayland a client cannot grab a global hotkey and the desktop shell —
// not us — draws the UI. The compositor binds the key and runs
// `mynah toggle`; the shell plugin runs `mynah watch` and draws what it
// hears. So this is a small line-delimited JSON server on a unix socket:
//
//     {"cmd": "toggle"}    -> {"ok": true, "state": "listening", "protocol": 2, "version": "..."}
//     {"cmd": "start"}     -> ... the same shape
//     {"cmd": "stop"}      -> {"ok": true, "state": "idle", "protocol": 2, ...}
//     {"cmd": "status"}    -> {"ok": true, "state": "idle", "pid": 4242, "protocol": 2, ...}
//     {"cmd": "quit"}      -> {"ok": true, ...}
//     {"cmd": "subscribe"} -> {"ok": true, "state": "idle", ...} then one
//                             line per event until the client goes away.
//
// Protocol v2 (P6): a strict superset of v1 — every v1 field keeps its
// shape, so the pinned Omarchy plugin works unchanged. Added, all
// ignorable by a v1 client: "protocol" and "version" on subscribe/status
// replies; {"event": "problem", "code", "message"}; {"event": "model",
// "status", "name", "tier"}.
//
// Anything that can write to this socket can make the machine dictate, and
// anything that can read it hears every word typed. So the socket lives in
// a 0700 directory under $XDG_RUNTIME_DIR (tmpfs, per-user, wiped at
// logout) with mode 0600, and the server refuses to start if that
// directory is not ours or is group/world-accessible — an attacker who
// wins the race for the path gets a socket nobody connects to, rather
// than one everyone does.
//
// Commands are acknowledged first and carried out afterwards: ending a
// session drains the transcription queue, and holding the reply for
// seconds would make the key that stopped dictation look wedged.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mynah::control {

// Where the socket lives. $MYNAH_SOCKET overrides; then $XDG_RUNTIME_DIR
// (tmpfs, 0700, per-user); then /tmp/mynah-<uid> which we create ourselves
// with the same permissions.
std::string socket_path();

// Raised when no running mynah answers on the control socket.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Server {
public:
    // What the commands do; `state` reports the engine's state as the
    // words the protocol has always used: "idle", "loading", "listening",
    // "transcribing".
    struct Handlers {
        std::function<void()> toggle;
        std::function<void()> start;
        std::function<void()> stop;
        std::function<void()> quit;
        std::function<std::string()> state;
    };

    Server(Handlers handlers, std::string path, std::string protocol_version);
    ~Server(); // stops serving and removes the socket

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Bind and serve in background threads. Throws Error when the socket
    // is taken by a live server, or when the directory is not ours.
    void start();

    // Stop serving, close subscribers, unlink the socket.
    void stop();

    const std::string& path() const { return path_; }

    // Send one event to every subscriber, dropping those that have gone.
    // A subscriber that has not read for 0.25 s is dropped: a slow shell
    // must never stall the audio thread.
    void publish(const std::string& event_json);

    // {"event": "level", "level": 0.42, "bands": [...]}, coalesced to at
    // most ~30 per second — a UI cannot show more than the compositor's
    // frame rate, and the socket buffer should not fill with stale
    // amplitudes while a subscriber is busy. bands_json is a pre-built
    // JSON array fragment, or empty when there are none.
    void publish_level(double level, const std::string& bands_json);

private:
    // The listening fd is handed to the accept thread rather than read from
    // server_: stop() owns server_, and a shared plain int written on one
    // thread while the other reads it is a data race (TSan) — worse, closing
    // it before the thread was joined let the fd number be reused by a new
    // connection that accept() was then called on.
    void accept_loop(int listening, int wake);
    void serve(int connection);

    // Every thread this server starts — one per connection, one per command
    // being carried out — is tracked, reaped once finished, and joined by
    // stop(). Handlers used to be detach()ed: a command still running when
    // the server was destroyed then called publish() on a dead object, and
    // `mynah toggle` followed by `mynah quit` could reach an engine already
    // destroyed. Connection threads were kept but never reaped, so a
    // long-running daemon collected one finished thread per `mynah status`.
    void spawn(std::function<void()> body);
    void reap_finished(); // joins finished workers; never waits on a live one
    // Registers the fd; whoever removes it from connections_ closes it,
    // exactly once (the serve thread on EOF, stop() on shutdown).
    void track_connection(int fd);
    void drop_connection(int fd);
    // Returns the reply JSON and whether the connection became a
    // subscriber.
    std::pair<std::string, bool> handle(const std::string& line);

    Handlers handlers_;
    std::string path_;
    std::string version_; // reported as "version" in v2 replies
    int server_ = -1;
    // The self-pipe that wakes the accept thread. Neither shutdown() nor
    // close() on a listening socket reliably wakes a blocked accept() on
    // every platform — shutdown does on Linux but not macOS, close does on
    // macOS but not Linux (and closing an fd another thread uses is the fd-
    // reuse bug) — so the thread poll()s the socket and this pipe, and
    // stop() writes one byte.
    int wake_pipe_[2] = {-1, -1};
    std::thread accept_thread_;
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex workers_mutex_; // guards workers_
    std::vector<Worker> workers_;
    std::mutex connections_mutex_; // guards connections_ and subscribers_
    std::vector<int> connections_;  // every live fd
    std::vector<int> subscribers_; // the subset that subscribed
    std::atomic<bool> stopping_{false};
    std::chrono::steady_clock::time_point last_level_at_;
};

// --- the client side, for `mynah toggle` and friends -----------------------

// Connect to a running mynah or throw Error with the same remedy the
// Python CLI printed.
int connect(const std::string& path, double timeout_seconds);

// Send one command, read one reply line, return it as raw JSON. Throws
// Error on the same conditions the Python client did, with the same
// messages.
std::string command(const std::string& cmd, double timeout_seconds = 5.0);

} // namespace mynah::control