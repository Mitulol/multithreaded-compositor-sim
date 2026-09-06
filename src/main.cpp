#include "surface_producer.hpp"
#include "compositor.hpp"
#include "event_queue.hpp"
#include "event_dispatcher.hpp"

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

int main(int argc, char** argv) {
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

    Compositor compositor(screenW, screenH, 60.0);
    compositor.addSurface(&s0, {0, 0});
    compositor.addSurface(&s1, {screenW / 2, 0});
    compositor.addSurface(&s2, {0, screenH / 2});
    compositor.addSurface(&s3, {screenW / 2, screenH / 2});

    EventQueue eventQueue;
    EventDispatcher dispatcher(eventQueue);

    s0.start(runFor);
    s1.start(runFor);
    s2.start(runFor);
    s3.start(runFor);
    dispatcher.start();

    // Input injector: a separate thread simulating a user generating
    // ~200 events/sec (touch/click/drag) at random surfaces, the way a
    // real input driver hands off events asynchronously to whatever
    // else the system is doing.
    std::thread injector([&] {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> surfaceDist(0, 3);
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
    s0.stop(); s1.stop(); s2.stop(); s3.stop();
    dispatcher.stop();

    std::cout << "=== compositor-sim summary (" << runFor.count() << "ms @ 60Hz vsync) ===\n\n";
    printStats("Compositor tick interval", compositor.tickIntervalStats().summarize());
    printStats("Input event latency", dispatcher.latencyStats().summarize());
    std::cout << "Input events handled: " << dispatcher.handledCount() << "\n\n";

    std::cout << std::left << std::setw(10) << "Surface"
               << std::setw(12) << "TargetFPS"
               << std::setw(12) << "Produced"
               << std::setw(10) << "Dropped"
               << "StaleReuse\n";
    SurfaceProducer* surfaces[] = {&s0, &s1, &s2, &s3};
    double fps[] = {24.0, 30.0, 60.0, 90.0};
    for (int i = 0; i < 4; ++i) {
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
