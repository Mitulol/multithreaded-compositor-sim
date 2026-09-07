#pragma once
#include "contended_mutex.hpp"  // for ContentionCounters (stays all-zero here)
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace comp {

// Lock-free single-producer / single-consumer "latest value wins" slot.
// Same contract as LatestSlot<T>: put() publishes, take() returns the
// freshest unconsumed value or nullopt, and a value overwritten before
// it was taken is counted as dropped.
//
// Implementation is a classic triple buffer -- exactly the scheme a real
// display pipeline uses to let a GPU keep rendering while the scanout
// hardware holds a stable front buffer. Three frame buffers, and a
// single atomic "mailbox" word that names the most-recently-published
// buffer plus a "fresh" bit:
//
//   * the producer owns writeIdx_, paints into it, then atomically
//     exchanges its index into the mailbox and recycles whatever buffer
//     was sitting there. exchange() never fails, so put() is wait-free.
//   * the consumer owns readIdx_; take() CAS-swaps its buffer into the
//     mailbox in exchange for the published one, clearing the fresh bit.
//     The CAS retries only if the producer published again in the gap,
//     so take() is lock-free.
//
// The three indices are always partitioned {producer-owned, mailbox,
// consumer-owned}, so no buffer is ever written and read at once.
template <typename T>
class TripleBufferLatestSlot {
public:
    TripleBufferLatestSlot() : mailbox_(pack(2, false)) {}

    void put(T value) {
        bufs_[writeIdx_] = std::move(value);
        uint32_t prev = mailbox_.exchange(pack(writeIdx_, true), std::memory_order_acq_rel);
        published_.fetch_add(1, std::memory_order_relaxed);
        if (fresh(prev)) {
            // There was already an unconsumed frame in the mailbox; it is
            // being discarded in favour of this newer one.
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        writeIdx_ = index(prev);
    }

    std::optional<T> take() {
        uint32_t cur = mailbox_.load(std::memory_order_acquire);
        for (;;) {
            if (!fresh(cur)) return std::nullopt;  // nothing new since last take()
            if (mailbox_.compare_exchange_weak(cur, pack(readIdx_, false),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                readIdx_ = index(cur);
                return std::move(bufs_[readIdx_]);
            }
            // cur was reloaded by the failed CAS; loop and re-check freshness.
        }
    }

    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t publishedCount() const { return published_.load(std::memory_order_relaxed); }

    // Present for interface parity with LatestSlot; a lock-free slot has
    // no mutex to contend on, so these counters stay zero.
    const ContentionCounters& contention() const { return counters_; }

private:
    static uint32_t pack(uint32_t idx, bool freshBit) { return (idx << 1) | (freshBit ? 1u : 0u); }
    static uint32_t index(uint32_t word) { return word >> 1; }
    static bool fresh(uint32_t word) { return word & 1u; }

    std::array<T, 3> bufs_{};
    std::atomic<uint32_t> mailbox_;
    uint32_t writeIdx_ = 0;  // producer-owned
    uint32_t readIdx_ = 1;   // consumer-owned
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> published_{0};
    ContentionCounters counters_;
};

} // namespace comp
