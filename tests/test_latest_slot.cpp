// Minimal test harness -- no external framework needed for a project
// this size. Uses an always-on CHECK macro rather than assert(), because
// assert() compiles to nothing under -DNDEBUG (which Release builds set)
// and would turn the whole suite into a no-op.
#include "latest_slot.hpp"
#include "triple_buffer_slot.hpp"
#include "event_queue.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

using namespace comp;

static int g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

#define RUN(fn)                              \
    do {                                     \
        int before = g_failures;             \
        fn();                                \
        if (g_failures == before)            \
            std::printf("PASS %s\n", #fn);   \
    } while (0)

// --- LatestSlot (mutex) -------------------------------------------------

static void test_take_empty_returns_nullopt() {
    LatestSlot<int> slot;
    CHECK(!slot.take().has_value());
}

static void test_put_then_take_roundtrips() {
    LatestSlot<int> slot;
    slot.put(42);
    auto v = slot.take();
    CHECK(v.has_value());
    CHECK(*v == 42);
    CHECK(!slot.take().has_value()); // consumed, nothing new
}

static void test_overwrite_counts_as_dropped() {
    LatestSlot<int> slot;
    slot.put(1);
    slot.put(2); // 1 is overwritten before being taken
    slot.put(3); // 2 is overwritten before being taken
    CHECK(slot.droppedCount() == 2);
    auto v = slot.take();
    CHECK(v.has_value());
    CHECK(*v == 3);
    CHECK(slot.publishedCount() == 3);
}

// --- TripleBufferLatestSlot (lock-free) --------------------------------
// Same behavioural contract as LatestSlot, so it gets the same tests.

static void test_tb_take_empty_returns_nullopt() {
    TripleBufferLatestSlot<int> slot;
    CHECK(!slot.take().has_value());
}

static void test_tb_put_then_take_roundtrips() {
    TripleBufferLatestSlot<int> slot;
    slot.put(42);
    auto v = slot.take();
    CHECK(v.has_value());
    CHECK(*v == 42);
    CHECK(!slot.take().has_value());
}

static void test_tb_overwrite_counts_as_dropped() {
    TripleBufferLatestSlot<int> slot;
    slot.put(1);
    slot.put(2);
    slot.put(3);
    CHECK(slot.droppedCount() == 2);
    auto v = slot.take();
    CHECK(v.has_value());
    CHECK(*v == 3);
    CHECK(slot.publishedCount() == 3);
}

// Stress: one producer, one consumer, no locks. Every value the consumer
// observes must be one the producer actually sent and must be
// monotonically increasing (the triple buffer must never hand back a
// torn or stale-then-newer frame). puts == takes + drops + still-in-slot.
static void test_tb_spsc_no_torn_or_reordered_values() {
    TripleBufferLatestSlot<uint64_t> slot;
    const uint64_t N = 2'000'000;

    std::thread producer([&] {
        for (uint64_t i = 1; i <= N; ++i) slot.put(i);
    });

    uint64_t last = 0, taken = 0;
    bool ordered = true;
    // Every value the producer sent is eventually either taken or
    // dropped, so this loop terminates once all N are accounted for.
    while (taken + slot.droppedCount() < N) {
        auto v = slot.take();
        if (v.has_value()) {
            if (*v <= last || *v > N) ordered = false;  // stale/torn/reordered
            last = *v;
            ++taken;
        }
    }
    producer.join();

    CHECK(ordered);
    CHECK(slot.publishedCount() == N);
    CHECK(taken + slot.droppedCount() == N);
}

// --- EventQueue ------------------------------------------------------

static void test_event_queue_fifo_order() {
    EventQueue q;
    q.push(InputEvent{1, 0, Clock::now()});
    q.push(InputEvent{2, 0, Clock::now()});
    auto a = q.pop();
    auto b = q.pop();
    CHECK(a.has_value());
    CHECK(a->surfaceId == 1);
    CHECK(b.has_value());
    CHECK(b->surfaceId == 2);
}

static void test_event_queue_shutdown_unblocks_pop() {
    EventQueue q;
    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        auto ev = q.pop();
        returned = !ev.has_value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.shutdown();
    waiter.join();
    CHECK(returned.load());
}

int main() {
    RUN(test_take_empty_returns_nullopt);
    RUN(test_put_then_take_roundtrips);
    RUN(test_overwrite_counts_as_dropped);
    RUN(test_tb_take_empty_returns_nullopt);
    RUN(test_tb_put_then_take_roundtrips);
    RUN(test_tb_overwrite_counts_as_dropped);
    RUN(test_tb_spsc_no_torn_or_reordered_values);
    RUN(test_event_queue_fifo_order);
    RUN(test_event_queue_shutdown_unblocks_pop);

    if (g_failures == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::printf("\n%d check(s) failed.\n", g_failures);
    return 1;
}
