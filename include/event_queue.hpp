#pragma once
#include "frame.hpp"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

namespace comp {

// A blocking FIFO queue for input events, handed off between the
// injector thread and a dedicated dispatcher thread. Keeping event
// handling on its own thread -- separate from both surface rendering
// and compositing -- means a busy compositor tick can never add
// latency to input response, mirroring why real windowing systems
// process input on a high-priority path independent of drawing.
class EventQueue {
public:
    void push(InputEvent ev) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(ev);
        }
        cv_.notify_one();
    }

    // Blocks until an event is available or the queue is shut down.
    std::optional<InputEvent> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        if (queue_.empty()) return std::nullopt;
        InputEvent ev = queue_.front();
        queue_.pop_front();
        return ev;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<InputEvent> queue_;
    bool shutdown_ = false;
};

} // namespace comp
