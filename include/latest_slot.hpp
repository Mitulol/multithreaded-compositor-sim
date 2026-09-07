#pragma once
#include "contended_mutex.hpp"
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
//
// This is the mutex-based implementation. include/triple_buffer_slot.hpp
// is a lock-free alternative with the same interface; bench/slot_bench.cpp
// measures the contention difference between the two.
template <typename T>
class LatestSlot {
public:
    LatestSlot() { mutex_.setCounters(&counters_); }

    void put(T value) {
        std::lock_guard<ContendedMutex> lock(mutex_);
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
        std::lock_guard<ContendedMutex> lock(mutex_);
        if (!hasValue_) return std::nullopt;
        hasValue_ = false;
        return std::move(value_);
    }

    uint64_t droppedCount() const {
        std::lock_guard<ContendedMutex> lock(mutex_);
        return dropped_;
    }

    uint64_t publishedCount() const {
        std::lock_guard<ContendedMutex> lock(mutex_);
        return published_;
    }

    // Measured lock contention on this slot (see contended_mutex.hpp).
    // Always present; only populated when built with
    // COMPOSITOR_SIM_INSTRUMENT.
    const ContentionCounters& contention() const { return counters_; }

private:
    mutable ContendedMutex mutex_;
    ContentionCounters counters_;
    std::optional<T> value_;
    bool hasValue_ = false;
    uint64_t dropped_ = 0;
    uint64_t published_ = 0;
};

} // namespace comp
