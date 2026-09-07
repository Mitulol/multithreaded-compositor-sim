// Contention benchmark: the mutex-based LatestSlot vs the lock-free
// TripleBufferLatestSlot, under an identical single-producer/single-
// consumer workload.
//
// Two payloads:
//   u64    - a plain 8-byte value. Isolates the raw cost of the
//            synchronisation itself (lock/unlock vs atomic exchange/CAS),
//            with no allocation in the critical section.
//   frame  - a real 160x120 premultiplied-RGBA Frame (~75 KiB). The
//            mutex slot move-assigns (and frees the displaced buffer)
//            while holding the lock; the triple buffer recycles a fixed
//            set of three buffers and never allocates after warm-up.
//
// Two rates:
//   hot    - producer and consumer spin with no delay: the slot is
//            hammered far past any real display rate. This is where lock
//            contention actually shows up.
//   real   - producer publishes at 90 fps, consumer takes at 60 Hz:
//            the rate the actual compositor runs at.
//
// Built with -DCOMPOSITOR_SIM_INSTRUMENT so LatestSlot's mutex records
// contention (see include/contended_mutex.hpp).

#include "frame.hpp"
#include "latest_slot.hpp"
#include "triple_buffer_slot.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

using namespace comp;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kSurfaceW = 160;
constexpr int kSurfaceH = 120;  // 160*120*4 = 76,800 B per frame buffer

struct Result {
    std::string impl, payload, rate;
    double seconds = 0.0;
    uint64_t puts = 0, takes = 0, empties = 0, drops = 0;
    uint64_t lockAcquires = 0, lockContended = 0, lockWaitNs = 0;
};

template <typename T> T makePayload(uint64_t seq);
template <> uint64_t makePayload<uint64_t>(uint64_t seq) { return seq; }
template <> Frame makePayload<Frame>(uint64_t seq) {
    Frame f(0, seq, kSurfaceW, kSurfaceH);
    f.rgba[(seq * 37) % f.rgba.size()] = static_cast<uint8_t>(seq);  // touch real memory
    return f;
}

template <template <typename> class SlotT, typename T>
Result run(const std::string& impl, const std::string& payload,
           const std::string& rate, bool hot, int ms) {
    SlotT<T> slot;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> takes{0}, empties{0};

    const auto producerPeriod = std::chrono::nanoseconds(1'000'000'000 / 90);
    const auto consumerPeriod = std::chrono::nanoseconds(1'000'000'000 / 60);

    std::thread producer([&] {
        uint64_t seq = 0;
        auto next = Clock::now();
        while (!stop.load(std::memory_order_relaxed)) {
            slot.put(makePayload<T>(++seq));
            if (!hot) { next += producerPeriod; std::this_thread::sleep_until(next); }
        }
    });
    std::thread consumer([&] {
        uint64_t t = 0, e = 0;
        auto next = Clock::now();
        while (!stop.load(std::memory_order_relaxed)) {
            if (slot.take().has_value()) ++t; else ++e;
            if (!hot) { next += consumerPeriod; std::this_thread::sleep_until(next); }
        }
        takes.store(t);
        empties.store(e);
    });

    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    stop.store(true);
    producer.join();
    consumer.join();
    auto t1 = Clock::now();

    Result r;
    r.impl = impl; r.payload = payload; r.rate = rate;
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.puts = slot.publishedCount();
    r.takes = takes.load();
    r.empties = empties.load();
    r.drops = slot.droppedCount();
    const auto& c = slot.contention();
    r.lockAcquires = c.acquisitions.load();
    r.lockContended = c.contended.load();
    r.lockWaitNs = c.waitNs.load();
    return r;
}

void printHeader() {
    std::printf("\n%-26s %-7s %-5s %12s %12s %10s %9s %11s %11s\n",
                "impl", "payload", "rate", "puts/s", "takes/s", "drops",
                "cont.%", "wait us/c", "totWait ms");
    std::printf("%s\n", std::string(116, '-').c_str());
}
void printResult(const Result& r) {
    double contPct = r.lockAcquires ? 100.0 * r.lockContended / r.lockAcquires : 0.0;
    double waitPerCont = r.lockContended ? (r.lockWaitNs / 1000.0) / r.lockContended : 0.0;
    std::printf("%-26s %-7s %-5s %12.0f %12.0f %10llu %8.3f%% %11.3f %11.2f\n",
                r.impl.c_str(), r.payload.c_str(), r.rate.c_str(),
                r.puts / r.seconds, r.takes / r.seconds,
                (unsigned long long)r.drops, contPct, waitPerCont, r.lockWaitNs / 1e6);
}

}  // namespace

int main(int argc, char** argv) {
    int ms = 2000;
    if (argc > 1) ms = std::atoi(argv[1]);

    std::printf("slot contention benchmark  (SPSC, %d ms per case)\n", ms);
#ifndef COMPOSITOR_SIM_INSTRUMENT
    std::printf("WARNING: built without COMPOSITOR_SIM_INSTRUMENT; contention columns will be zero\n");
#endif
    printHeader();
    printResult(run<LatestSlot, uint64_t>("LatestSlot (std::mutex)", "u64", "hot", true, ms));
    printResult(run<TripleBufferLatestSlot, uint64_t>("TripleBuffer (lock-free)", "u64", "hot", true, ms));
    printResult(run<LatestSlot, Frame>("LatestSlot (std::mutex)", "frame", "hot", true, ms));
    printResult(run<TripleBufferLatestSlot, Frame>("TripleBuffer (lock-free)", "frame", "hot", true, ms));
    printResult(run<LatestSlot, Frame>("LatestSlot (std::mutex)", "frame", "real", false, ms));
    printResult(run<TripleBufferLatestSlot, Frame>("TripleBuffer (lock-free)", "frame", "real", false, ms));
    std::printf("\n");
    return 0;
}
