// The typing queue: the engine's TEXT callback must return at once, the
// texts must land in the order spoken, and quitting must not drop the last.

#include "vendor/doctest.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "typing_queue.hpp"

using namespace std::chrono_literals;

TEST_CASE("push returns at once while the typist is busy") {
    std::atomic<bool> release{false};
    mynah::inject::TypingQueue queue(
        [&](const std::string&) {
            while (!release.load()) std::this_thread::sleep_for(1ms);
            return true;
        },
        nullptr);
    auto began = std::chrono::steady_clock::now();
    queue.push("one"); // the typist takes this and blocks on it
    queue.push("two");
    queue.push("three");
    CHECK(std::chrono::steady_clock::now() - began < 100ms);
    release.store(true);
}

TEST_CASE("texts land in order, and only those that landed are reported") {
    std::mutex mutex;
    std::vector<std::string> typed;
    std::vector<std::string> landed;
    {
        mynah::inject::TypingQueue queue(
            [&](const std::string& text) {
                std::lock_guard<std::mutex> lock(mutex);
                typed.push_back(text);
                return text != "fails";
            },
            [&](const std::string& text) {
                std::lock_guard<std::mutex> lock(mutex);
                landed.push_back(text);
            });
        for (const char* text : {"первое", "fails", "третье"}) queue.push(text);
    } // destruction drains the queue
    CHECK(typed == std::vector<std::string>{"первое", "fails", "третье"});
    CHECK(landed == std::vector<std::string>{"первое", "третье"});
}

TEST_CASE("quitting types what is still queued") {
    std::atomic<int> count{0};
    {
        mynah::inject::TypingQueue queue(
            [&](const std::string&) {
                std::this_thread::sleep_for(20ms);
                ++count;
                return true;
            },
            nullptr);
        for (int i = 0; i < 5; ++i) queue.push("x");
    }
    CHECK(count.load() == 5);
}
