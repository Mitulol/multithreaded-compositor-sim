#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <mutex>

namespace comp {

// Thread-safe accumulator for a stream of durations (nanoseconds).
// Used both for compositor tick jitter and for input event latency.
class DurationStats {
public:
    void record(double nanoseconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.push_back(nanoseconds);
    }

    struct Summary {
        size_t count = 0;
        double meanMs = 0.0;
        double stddevMs = 0.0;
        double minMs = 0.0;
        double maxMs = 0.0;
        double p99Ms = 0.0;
    };

    Summary summarize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Summary s;
        s.count = samples_.size();
        if (samples_.empty()) return s;

        std::vector<double> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());

        double sum = 0.0;
        for (double v : sorted) sum += v;
        double mean = sum / sorted.size();

        double sqSum = 0.0;
        for (double v : sorted) sqSum += (v - mean) * (v - mean);
        double variance = sorted.size() > 1 ? sqSum / (sorted.size() - 1) : 0.0;

        size_t p99Index = static_cast<size_t>(0.99 * (sorted.size() - 1));

        s.meanMs = mean / 1e6;
        s.stddevMs = std::sqrt(variance) / 1e6;
        s.minMs = sorted.front() / 1e6;
        s.maxMs = sorted.back() / 1e6;
        s.p99Ms = sorted[p99Index] / 1e6;
        return s;
    }

private:
    mutable std::mutex mutex_;
    std::vector<double> samples_;
};

} // namespace comp
