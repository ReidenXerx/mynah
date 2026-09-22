// mynah::inject::TypingQueue — text events typed off the engine's thread.
//
// The engine delivers TEXT on its own threads, and mynah.h's contract is that
// callbacks never block. Typing does block: a wtype or wl-copy child, a paste
// chord, up to the injector's 10 s kill timeout. So the callback only
// enqueues, and one thread types, in the order the utterances were spoken.
//
// Destruction types whatever is still queued, then joins: the last sentence
// of a session that ends with `mynah quit` still lands.

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace mynah::inject {

class TypingQueue {
public:
    // `type` does the typing and says whether the text landed; `landed`
    // runs after each text that did (the CLI publishes the `text` event
    // there, so the event means "typed", as it always has).
    TypingQueue(std::function<bool(const std::string&)> type,
                std::function<void(const std::string&)> landed)
        : type_(std::move(type)), landed_(std::move(landed)), worker_([this] { run(); }) {}

    ~TypingQueue() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        worker_.join();
    }

    TypingQueue(const TypingQueue&) = delete;
    TypingQueue& operator=(const TypingQueue&) = delete;

    // Any thread; returns at once.
    void push(std::string text) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(text));
        }
        wake_.notify_one();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return; // stopping, and nothing left to type
            std::string text = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            if (type_(text) && landed_) landed_(text);
            lock.lock();
        }
    }

    std::function<bool(const std::string&)> type_;
    std::function<void(const std::string&)> landed_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    bool stopping_ = false;
    std::thread worker_; // last: starts once everything above exists
};

} // namespace mynah::inject
