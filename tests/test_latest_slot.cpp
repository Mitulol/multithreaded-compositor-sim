// Minimal assert-based test harness -- no external test framework
// dependency needed for a project this size.
#include "latest_slot.hpp"
#include "event_queue.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

using namespace comp;

static void test_take_empty_returns_nullopt() {
    LatestSlot<int> slot;
    assert(!slot.take().has_value());
    std::cout << "PASS test_take_empty_returns_nullopt\n";
}

static void test_put_then_take_roundtrips() {
    LatestSlot<int> slot;
    slot.put(42);
    auto v = slot.take();
    assert(v.has_value());
    assert(*v == 42);
    assert(!slot.take().has_value()); // consumed, nothing new
    std::cout << "PASS test_put_then_take_roundtrips\n";
}

static void test_overwrite_counts_as_dropped() {
    LatestSlot<int> slot;
    slot.put(1);
    slot.put(2); // 1 is overwritten before being taken
    slot.put(3); // 2 is overwritten before being taken
    assert(slot.droppedCount() == 2);
    auto v = slot.take();
    assert(v.has_value() && *v == 3);
    assert(slot.publishedCount() == 3);
    std::cout << "PASS test_overwrite_counts_as_dropped\n";
}

static void test_event_queue_fifo_order() {
    EventQueue q;
    q.push(InputEvent{1, 0, Clock::now()});
    q.push(InputEvent{2, 0, Clock::now()});
    auto a = q.pop();
    auto b = q.pop();
    assert(a.has_value() && a->surfaceId == 1);
    assert(b.has_value() && b->surfaceId == 2);
    std::cout << "PASS test_event_queue_fifo_order\n";
}

static void test_event_queue_shutdown_unblocks_pop() {
    EventQueue q;
    bool returned = false;
    std::thread waiter([&] {
        auto ev = q.pop();
        returned = !ev.has_value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.shutdown();
    waiter.join();
    assert(returned);
    std::cout << "PASS test_event_queue_shutdown_unblocks_pop\n";
}

int main() {
    test_take_empty_returns_nullopt();
    test_put_then_take_roundtrips();
    test_overwrite_counts_as_dropped();
    test_event_queue_fifo_order();
    test_event_queue_shutdown_unblocks_pop();
    std::cout << "\nAll tests passed.\n";
    return 0;
}
