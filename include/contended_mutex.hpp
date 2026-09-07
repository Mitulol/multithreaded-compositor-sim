#pragma once
#include <atomic>
#include <chrono>
#include <mutex>

namespace comp {

// A drop-in replacement for std::mutex that records how often a lock
// acquisition was actually contended (i.e. the lock was already held and
// the caller had to wait) and how long callers spent blocked.
//
// This exists so the simulation can report *measured* contention on the
// LatestSlot lock rather than guessed contention. The fast, uncontended
// path costs one extra try_lock() and a couple of relaxed atomic adds;
// the timed clock reads only happen once the try_lock() has already
// failed, so an uncontended run is barely perturbed by the measurement.
//
// The whole facility is compiled out unless COMPOSITOR_SIM_INSTRUMENT is
// defined, so the default build keeps std::mutex semantics with zero
// overhead and no extra atomics.
struct ContentionCounters {
    std::atomic<uint64_t> acquisitions{0};   // total lock() calls
    std::atomic<uint64_t> contended{0};       // lock() calls that had to wait
    std::atomic<uint64_t> waitNs{0};          // cumulative nanoseconds blocked

    void reset() {
        acquisitions.store(0, std::memory_order_relaxed);
        contended.store(0, std::memory_order_relaxed);
        waitNs.store(0, std::memory_order_relaxed);
    }

    double contentionRate() const {
        uint64_t a = acquisitions.load(std::memory_order_relaxed);
        return a ? static_cast<double>(contended.load(std::memory_order_relaxed)) / a : 0.0;
    }
    double meanWaitNsPerContended() const {
        uint64_t c = contended.load(std::memory_order_relaxed);
        return c ? static_cast<double>(waitNs.load(std::memory_order_relaxed)) / c : 0.0;
    }
};

#ifdef COMPOSITOR_SIM_INSTRUMENT

class ContendedMutex {
public:
    explicit ContendedMutex(ContentionCounters* counters = nullptr) : counters_(counters) {}
    void setCounters(ContentionCounters* c) { counters_ = c; }

    void lock() {
        if (!counters_) { mutex_.lock(); return; }
        counters_->acquisitions.fetch_add(1, std::memory_order_relaxed);
        if (mutex_.try_lock()) return;  // uncontended: no clock reads at all
        counters_->contended.fetch_add(1, std::memory_order_relaxed);
        auto t0 = std::chrono::steady_clock::now();
        mutex_.lock();
        auto t1 = std::chrono::steady_clock::now();
        counters_->waitNs.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
            std::memory_order_relaxed);
    }
    void unlock() { mutex_.unlock(); }
    bool try_lock() {
        if (counters_) counters_->acquisitions.fetch_add(1, std::memory_order_relaxed);
        return mutex_.try_lock();
    }

private:
    std::mutex mutex_;
    ContentionCounters* counters_ = nullptr;
};

#else  // !COMPOSITOR_SIM_INSTRUMENT

class ContendedMutex {
public:
    explicit ContendedMutex(ContentionCounters* = nullptr) {}
    void setCounters(ContentionCounters*) {}
    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }
    bool try_lock() { return mutex_.try_lock(); }

private:
    std::mutex mutex_;
};

#endif

} // namespace comp
