#pragma once
#include "event_queue.hpp"
#include "metrics.hpp"
#include <thread>
#include <atomic>

namespace comp {

// Dedicated consumer thread for input events. Measures end-to-end
// latency from injection to dispatch so the simulation can report
// whether the event path stays responsive independent of compositor
// or surface-producer load.
class EventDispatcher {
public:
    explicit EventDispatcher(EventQueue& queue) : queue_(queue) {}

    void start() {
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        queue_.shutdown();
        if (thread_.joinable()) thread_.join();
    }

    const DurationStats& latencyStats() const { return latency_; }
    uint64_t handledCount() const { return handled_; }

private:
    void run() {
        while (true) {
            auto ev = queue_.pop();
            if (!ev.has_value()) break; // shutdown
            auto now = Clock::now();
            double latencyNs = std::chrono::duration<double, std::nano>(now - ev->injectedAt).count();
            latency_.record(latencyNs);
            ++handled_;
        }
    }

    EventQueue& queue_;
    std::thread thread_;
    DurationStats latency_;
    std::atomic<uint64_t> handled_{0};
};

} // namespace comp
