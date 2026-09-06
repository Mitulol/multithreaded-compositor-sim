#pragma once
#include <mutex>
#include <optional>
#include <cstdint>

namespace comp {

// A depth-1 buffer slot: a producer publishes a value, a consumer takes
// the most recent one. If the producer outpaces the consumer, the
// previous unconsumed value is overwritten (and counted as dropped)
// rather than queued. This is deliberately the same tradeoff a real
// compositor's buffer queue makes when a client renders faster than
// the display refreshes: showing the newest frame beats showing every
// frame, because a backlog of stale frames only adds latency.
template <typename T>
class LatestSlot {
public:
    void put(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hasValue_) {
            ++dropped_;
        }
        value_ = std::move(value);
        hasValue_ = true;
        ++published_;
    }

    // Returns the freshest value if one has arrived since the last
    // take(), or nullopt if the producer hasn't published anything new
    // (in which case the caller should keep reusing its last frame).
    std::optional<T> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasValue_) return std::nullopt;
        hasValue_ = false;
        return std::move(value_);
    }

    uint64_t droppedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    uint64_t publishedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return published_;
    }

private:
    mutable std::mutex mutex_;
    std::optional<T> value_;
    bool hasValue_ = false;
    uint64_t dropped_ = 0;
    uint64_t published_ = 0;
};

} // namespace comp
