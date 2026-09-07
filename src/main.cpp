#include "surface_producer.hpp"
#include "compositor.hpp"
#include "event_queue.hpp"
#include "event_dispatcher.hpp"
#include "frame_slot.hpp"

#include <iostream>
#include <iomanip>
#include <random>
#include <thread>
#include <sys/stat.h>

using namespace comp;
using namespace std::chrono_literals;

static void ensureDir(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

static void printStats(const char* label, const DurationStats::Summary& s) {
    std::cout << std::left << std::setw(24) << label
               << "n=" << std::setw(6) << s.count
               << " mean=" << std::fixed << std::setprecision(3) << std::setw(8) << s.meanMs << "ms"
               << " stddev=" << std::setw(8) << s.stddevMs << "ms"
               << " min=" << std::setw(8) << s.minMs << "ms"
               << " max=" << std::setw(8) << s.maxMs << "ms"
               << " p99=" << s.p99Ms << "ms\n";
}

int main() {
    const auto runFor = 5000ms;
    const int screenW = 320, screenH = 240;
    ensureDir("frames");

    // Four surfaces at different, deliberately mismatched frame rates
    // versus the 60Hz display -- one slower (24fps, e.g. a video), two
    // matched-ish (30/60fps), one faster than the display can show
    // (90fps), so the compositor's drop/reuse behavior is exercised in
    // both directions.
    SurfaceProducer s0(0, screenW / 2, screenH / 2, 24.0, {200, 60, 60});
    SurfaceProducer s1(1, screenW / 2, screenH / 2, 30.0, {60, 200, 60});
    SurfaceProducer s2(2, screenW / 2, screenH / 2, 60.0, {60, 60, 200});
    SurfaceProducer s3(3, screenW / 2, screenH / 2, 90.0, {200, 200, 60});

    // A fifth, translucent overlay spanning the middle of the screen at
    // 45% opacity -- a heads-up / notification layer. It overlaps all
    // four grid surfaces, so the composited output shows real "source
    // over" blending rather than opaque tiles.
    SurfaceProducer overlay(4, screenW, screenH / 3, 30.0, {240, 240, 255}, /*alpha=*/0.45);

    Compositor compositor(screenW, screenH, 60.0);
    compositor.addSurface(&s0, {0, 0});
    compositor.addSurface(&s1, {screenW / 2, 0});
    compositor.addSurface(&s2, {0, screenH / 2});
    compositor.addSurface(&s3, {screenW / 2, screenH / 2});
    compositor.addSurface(&overlay, {0, screenH / 3});  // drawn last => on top

    EventQueue eventQueue;
    EventDispatcher dispatcher(eventQueue);

    SurfaceProducer* surfaces[] = {&s0, &s1, &s2, &s3, &overlay};
    double fps[] = {24.0, 30.0, 60.0, 90.0, 30.0};

    for (auto* s : surfaces) s->start(runFor);
    dispatcher.start();

    // Input injector: a separate thread simulating a user generating
    // ~200 events/sec (touch/click/drag) at random surfaces, the way a
    // real input driver hands off events asynchronously to whatever
    // else the system is doing.
    std::thread injector([&] {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> surfaceDist(0, 4);
        std::uniform_int_distribution<int> typeDist(0, 2);
        const auto start = Clock::now();
        const auto deadline = start + runFor;
        auto next = start;
        while (Clock::now() < deadline) {
            eventQueue.push(InputEvent{surfaceDist(rng), typeDist(rng), Clock::now()});
            next += std::chrono::microseconds(5000); // ~200 Hz
            std::this_thread::sleep_until(next);
        }
    });

    compositor.run(runFor + 200ms, /*dumpEveryNTicks=*/30, "frames");

    injector.join();
    for (auto* s : surfaces) s->stop();
    dispatcher.stop();

    std::cout << "=== compositor-sim summary (" << runFor.count() << "ms @ 60Hz vsync) ===\n";
    std::cout << "frame slot impl: " << kFrameSlotImpl << "\n\n";
    printStats("Compositor tick interval", compositor.tickIntervalStats().summarize());
    printStats("Input event latency", dispatcher.latencyStats().summarize());
    std::cout << "Input events handled: " << dispatcher.handledCount() << "\n\n";

    std::cout << std::left << std::setw(10) << "Surface"
               << std::setw(12) << "TargetFPS"
               << std::setw(12) << "Produced"
               << std::setw(10) << "Dropped"
               << "StaleReuse\n";
    for (int i = 0; i < 5; ++i) {
        auto* s = surfaces[i];
        std::cout << std::left << std::setw(10) << s->id()
                   << std::setw(12) << fps[i]
                   << std::setw(12) << s->slot().publishedCount()
                   << std::setw(10) << s->slot().droppedCount()
                   << compositor.staleReusesFor(s->id()) << "\n";
    }

    std::cout << "\nPPM frame snapshots written to ./frames/\n";
    return 0;
}
