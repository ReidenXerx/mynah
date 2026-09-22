// The control socket: the port of the socket tests in tests/test_linux.py,
// which ran against the Python engine's control.py. The Omarchy plugin at
// its pinned commit drives THIS server, so every reply shape here is
// load-bearing — and protocol v2 (P6) must stay a strict superset: the
// pinned plugin ignores the added fields.
//
// Runs on any POSIX platform; nothing here needs Wayland or PipeWire.

#include "vendor/doctest.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "control_socket.hpp"
#include "env.hpp"
#include "json.hpp"

using mynah::control::Server;

namespace {

// Records what the socket asked the engine to do (test_linux.py's
// FakeEngine).
struct FakeEngine {
    std::mutex mutex;
    std::vector<std::string> calls;
    std::string state_ = "idle"; // written by handler threads: guarded

    void record(const std::string& name, const std::string& next_state) {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(name);
        if (!next_state.empty()) state_ = next_state;
    }
    std::string state() {
        std::lock_guard<std::mutex> lock(mutex);
        return state_;
    }
    std::vector<std::string> take_calls() {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
    // Commands are acknowledged first and carried out afterwards, on their
    // own thread — so a test waits for the call instead of assuming it.
    bool await_calls(std::size_t n) {
        for (int i = 0; i < 400; ++i) {
            if (take_calls().size() >= n) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

// A raw client, for the tests that need their own connection. Two things the
// hand-rolled versions got wrong: a receive timeout, so a missing reply FAILS
// the test instead of hanging the whole suite forever; and a send that
// computes its own length — two tests wrote 20 bytes of the 21-byte
// subscribe line, dropped the newline, and waited for a reply to a command
// the server never received.
int connect_client(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    struct timeval timeout{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

void send_line(int fd, const std::string& text) {
    std::string line = text + "\n";
    REQUIRE(::write(fd, line.data(), line.size()) == ssize_t(line.size()));
}

Server::Handlers handlers_for(FakeEngine* engine,
                               std::function<void()> quit = nullptr) {
    Server::Handlers handlers;
    handlers.toggle = [engine] {
        engine->record("toggle", engine->state() == "idle" ? "listening" : "idle");
    };
    handlers.start = [engine] { engine->record("start", "listening"); };
    handlers.stop = [engine] { engine->record("stop", "idle"); };
    handlers.quit = [engine, quit = std::move(quit)] {
        engine->record("quit", "");
        if (quit) quit();
    };
    handlers.state = [engine] { return engine->state(); };
    return handlers;
}

struct Fixture {
    mynah_test::TmpDir dir;
    FakeEngine engine;
    std::unique_ptr<Server> server;

    Fixture() {
        std::filesystem::path path = dir.path() / "sock" / "control.sock";
        server = std::make_unique<Server>(handlers_for(&engine), path.string(), "test");
        server->start();
    }
    ~Fixture() { server->stop(); }

    std::string path() const { return server->path(); }

    // Send messages on one connection, read `expect` reply lines.
    std::vector<mynah::json::Value> talk(const std::vector<std::string>& messages,
                                         int expect = 1) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        struct timeval timeout{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path().c_str(), sizeof(address.sun_path) - 1);
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        for (const std::string& message : messages) {
            std::string line = message + "\n";
            REQUIRE(::write(fd, line.data(), line.size()) > 0);
        }
        std::vector<mynah::json::Value> replies;
        std::string buffer;
        char chunk[4096];
        while (replies.size() < std::size_t(expect)) {
            ssize_t n = ::read(fd, chunk, sizeof(chunk));
            if (n <= 0) break;
            buffer.append(chunk, std::size_t(n));
            std::size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (line.empty()) continue;
                replies.push_back(mynah::json::parse(line));
                if (replies.size() == std::size_t(expect)) break;
            }
        }
        ::close(fd);
        return replies;
    }
};

} // namespace

TEST_CASE("commands reach the engine") {
    Fixture fixture;
    auto replies = fixture.talk({"{\"cmd\": \"toggle\"}"});
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].find("ok")->boolean == true);
    // The reply is an acknowledgement sent BEFORE the command runs, so the
    // state in it is whatever the engine was when it answered — it cannot
    // promise "listening". What happened arrives as state events. (Python's
    // twin asserted "listening" and passed by winning a thread race.)
    const std::string state = replies[0].find("state")->text;
    CHECK((state == "idle" || state == "loading" || state == "listening" ||
           state == "transcribing"));
    REQUIRE(fixture.engine.await_calls(1));
    CHECK(fixture.engine.take_calls() == std::vector<std::string>{"toggle"});
}

TEST_CASE("a bare word is a command too") {
    // `echo toggle | socat - UNIX:...` must work without quoting JSON.
    Fixture fixture;
    auto replies = fixture.talk({"stop"});
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].find("ok")->boolean == true);
    // Acknowledged first, run afterwards — wait for it, as above.
    REQUIRE(fixture.engine.await_calls(1));
    CHECK(fixture.engine.take_calls() == std::vector<std::string>{"stop"});
}

TEST_CASE("status reports state and pid") {
    Fixture fixture;
    auto replies = fixture.talk({"{\"cmd\": \"status\"}"});
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].find("state")->text == "idle");
    CHECK(int(replies[0].find("pid")->number) == ::getpid());
}

TEST_CASE("v2 replies carry protocol and version; v1 shapes survive") {
    Fixture fixture;
    auto replies = fixture.talk({"{\"cmd\": \"status\"}"});
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].find("protocol")->number == 2);
    CHECK(!replies[0].find("version")->text.empty());
    // The v1 fields are all still there, unchanged.
    for (const char* key : {"ok", "state", "pid"})
        CHECK(replies[0].find(key) != nullptr);
}

TEST_CASE("an unknown command is refused, not ignored") {
    Fixture fixture;
    auto replies = fixture.talk({"{\"cmd\": \"selfdestruct\"}"});
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].find("ok")->boolean == false);
    CHECK(replies[0].find("error")->text.find("selfdestruct") != std::string::npos);
    CHECK(fixture.engine.take_calls().empty());
}

TEST_CASE("a failing handler does not break the socket") {
    // The reply is an acknowledgement, so a handler that raises afterwards
    // reaches subscribers as an error event rather than as a broken
    // connection.
    mynah_test::TmpDir dir;
    Server::Handlers handlers;
    handlers.toggle = [] { throw std::runtime_error("no microphone"); };
    handlers.start = handlers.stop = handlers.quit = handlers.toggle;
    handlers.state = [] { return std::string("idle"); };
    mynah::control::Server server(std::move(handlers),
                                  (dir.path() / "s" / "control.sock").string(), "test");
    server.start();

    // Subscribe first, so the error event has somewhere to arrive.
    int fd = connect_client(server.path());
    send_line(fd, "{\"cmd\": \"subscribe\"}");
    std::string reply_line = mynah_test::read_line(fd);
    REQUIRE(!reply_line.empty());
    CHECK(mynah::json::parse(reply_line).find("ok")->boolean == true);

    // Ask for the toggle that explodes; the reply is the acknowledgement…
    int client = connect_client(server.path());
    send_line(client, "toggle");
    std::string ack = mynah_test::read_line(client);
    REQUIRE(!ack.empty());
    CHECK(mynah::json::parse(ack).find("ok")->boolean == true);
    ::close(client);

    // …and the failure arrives on the subscriber as an error event.
    std::string event = mynah_test::read_line(fd);
    REQUIRE(!event.empty());
    auto parsed = mynah::json::parse(event);
    CHECK(parsed.find("event")->text == "error");
    CHECK(parsed.find("command")->text == "toggle");
    CHECK(parsed.find("error")->text == "no microphone");
    ::close(fd);

    server.stop();
}

TEST_CASE("a slow handler does not hold the reply") {
    // Ending a session drains the transcription queue — seconds of
    // whisper. Holding the reply for that makes the key that stopped
    // dictation look wedged, and the client times out with nothing to
    // show for it.
    mynah_test::TmpDir dir;
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    Server::Handlers handlers;
    handlers.stop = [&] {
        started.store(true);
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    handlers.toggle = handlers.start = handlers.quit = handlers.stop;
    handlers.state = [] { return std::string("listening"); };
    mynah::control::Server server(std::move(handlers),
                                  (dir.path() / "s" / "control.sock").string(), "test");
    server.start();

    auto began = std::chrono::steady_clock::now();
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    struct timeval timeout{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::string path = server.path();
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::write(fd, "stop\n", 5) > 0);
    std::string ack = mynah_test::read_line(fd);
    double answered_in =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    CHECK(mynah::json::parse(ack).find("ok")->boolean == true);
    CHECK_MESSAGE(answered_in < 1.0,
                  ("the reply waited " + std::to_string(answered_in) +
                   "s for the handler")
                      .c_str());
    ::close(fd);
    // Acknowledged first, run afterwards: the handler may not have begun yet.
    for (int i = 0; i < 200 && !started.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(started.load());
    release.store(true);
    server.stop();
}

TEST_CASE("subscribers receive events") {
    Fixture fixture;
    int fd = connect_client(fixture.path());
    send_line(fd, "{\"cmd\": \"subscribe\"}");
    std::string ack = mynah_test::read_line(fd);
    REQUIRE(!ack.empty());
    CHECK(mynah::json::parse(ack).find("ok")->boolean == true);

    // No sleep: the server registers a subscriber before it sends the ack,
    // so an event published the moment the ack is read cannot be missed.
    fixture.server->publish("{\"event\":\"state\",\"state\":\"listening\"}");
    std::string event = mynah_test::read_line(fd);
    REQUIRE(!event.empty());
    CHECK(mynah::json::parse(event).find("state")->text == "listening");
    ::close(fd);
}

TEST_CASE("publishing with nobody listening is a no-op") {
    mynah_test::TmpDir dir;
    Server::Handlers handlers;
    mynah::control::Server server(std::move(handlers),
                                  (dir.path() / "s" / "control.sock").string(), "test");
    server.start();
    CHECK_NOTHROW(server.publish("{\"event\":\"state\",\"state\":\"listening\"}"));
    CHECK_NOTHROW(server.publish_level(0.5, ""));
    server.stop();
}

TEST_CASE("the socket is private") {
    Fixture fixture;
    struct stat info{};
    REQUIRE(::stat(fixture.path().c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
    std::filesystem::path directory = std::filesystem::path(fixture.path()).parent_path();
    REQUIRE(::stat(directory.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0700);
}

TEST_CASE("a world-writable directory is refused") {
    // Someone who can write the directory can swap the socket for their
    // own, and then every `mynah watch` in the session is talking to them.
    mynah_test::TmpDir dir;
    std::filesystem::path shared = dir.path() / "shared";
    std::filesystem::create_directories(shared);
    ::chmod(shared.c_str(), 0777);
    FakeEngine engine;
    mynah::control::Server server(handlers_for(&engine),
                                  (shared / "control.sock").string(), "test");
    bool refused = false;
    try {
        server.start();
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

TEST_CASE("a stale socket is replaced") {
    // A killed mynah leaves the socket file behind; the next one must bind.
    mynah_test::TmpDir dir;
    std::filesystem::path directory = dir.path() / "run";
    std::filesystem::create_directories(directory);
    // 0700, as the Python twin makes it (mkdir(mode=0o700)): the server
    // refuses a directory other users can reach, and this one is ours.
    ::chmod(directory.c_str(), 0700);
    std::filesystem::path path = directory / "control.sock";
    std::ofstream(path) << "stale";
    FakeEngine engine;
    mynah::control::Server server(handlers_for(&engine), path.string(), "test");
    server.start();
    CHECK(server.path() == path.string());
    // And it answers — the stale file really was replaced by a live socket.
    int fd = connect_client(path.string());
    send_line(fd, "status");
    std::string reply = mynah_test::read_line(fd);
    REQUIRE(!reply.empty());
    CHECK(mynah::json::parse(reply).find("ok")->boolean == true);
    ::close(fd);
    server.stop();
}

TEST_CASE("a live socket is not stolen") {
    // Two mynahs must not fight over one socket: the second refuses.
    Fixture fixture;
    FakeEngine second_engine;
    mynah::control::Server second(handlers_for(&second_engine), fixture.path(), "test");
    bool refused = false;
    std::string message;
    try {
        second.start();
    } catch (const std::exception& e) {
        refused = true;
        message = e.what();
    }
    CHECK(refused);
    CHECK(message.find("already running") != std::string::npos);
}

TEST_CASE("stop removes the socket") {
    mynah_test::TmpDir dir;
    FakeEngine engine;
    std::filesystem::path path = dir.path() / "run" / "control.sock";
    mynah::control::Server server(handlers_for(&engine), path.string(), "test");
    server.start();
    server.stop();
    CHECK(!std::filesystem::exists(path));
}

TEST_CASE("the socket path prefers the runtime dir") {
    mynah_test::EnvOverride clear_socket("MYNAH_SOCKET", "");
    mynah_test::EnvOverride runtime("XDG_RUNTIME_DIR", "/run/user/4242");
    CHECK(mynah::control::socket_path() == "/run/user/4242/mynah/control.sock");
    {
        mynah_test::EnvOverride no_runtime("XDG_RUNTIME_DIR", "");
        CHECK(mynah::control::socket_path() ==
              "/tmp/mynah-" + std::to_string(::getuid()) + "/control.sock");
    }
}

TEST_CASE("the client says how to start one") {
    mynah_test::TmpDir dir;
    mynah_test::EnvOverride socket("MYNAH_SOCKET",
                                   (dir.path() / "nothing.sock").string());
    bool threw = false;
    std::string message;
    try {
        (void)mynah::control::command("status");
    } catch (const mynah::control::Error& e) {
        threw = true;
        message = e.what();
    }
    CHECK(threw);
    CHECK(message.find("no running mynah") != std::string::npos);
    CHECK(message.find("systemctl --user start mynah") != std::string::npos);
}
TEST_CASE("a socket path too long for a unix socket is refused, not truncated") {
    // sun_path holds 104 bytes on macOS and 108 on Linux. A longer path was
    // strncpy'd in and cut short: the server bound somewhere else, and
    // every client reported "no running mynah" with no hint why.
    mynah_test::TmpDir dir;
    std::filesystem::path deep = dir.path();
    while (deep.string().size() < 120) deep /= "a-directory-with-a-long-name";
    std::filesystem::create_directories(deep);
    ::chmod(deep.c_str(), 0700);
    std::string path = (deep / "control.sock").string();

    FakeEngine engine;
    mynah::control::Server server(handlers_for(&engine), path, "test");
    try {
        server.start();
        FAIL("start() accepted a path that does not fit sun_path");
    } catch (const mynah::control::Error& e) {
        CHECK(std::string(e.what()).find("too long") != std::string::npos);
    }
    // Nothing may have been bound at a truncated path instead.
    CHECK(!std::filesystem::exists(path));

    try {
        mynah::control::connect(path, 0.5);
        FAIL("connect() accepted a path that does not fit sun_path");
    } catch (const mynah::control::Error& e) {
        CHECK(std::string(e.what()).find("too long") != std::string::npos);
    }
}
