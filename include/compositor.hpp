#pragma once
#include "frame.hpp"
#include "surface_producer.hpp"
#include "metrics.hpp"
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include <fstream>
#include <unordered_map>

namespace comp {

struct Placement { int x, y; };

// The compositor itself: a single thread paced by a fixed-rate vsync
// timer (analogous to a display's refresh interrupt). On every tick it
// pulls the freshest available frame from each surface's LatestSlot,
// reuses the previous frame for any surface that has nothing new
// (frame persistence, same as a real display holding the last image
// during a stalled app), blends them into one framebuffer at their
// assigned screen positions, and records scheduling metrics.
class Compositor {
public:
    Compositor(int screenW, int screenH, double vsyncHz)
        : screenW_(screenW), screenH_(screenH),
          period_(std::chrono::duration_cast<Clock::duration>(
              std::chrono::duration<double>(1.0 / vsyncHz))) {}

    void addSurface(SurfaceProducer* surface, Placement placement) {
        surfaces_.push_back(surface);
        placements_[surface->id()] = placement;
    }

    void run(std::chrono::milliseconds runFor, int dumpEveryNTicks, const std::string& frameDir) {
        std::vector<uint8_t> framebuffer(static_cast<size_t>(screenW_) * screenH_ * 3, 32);
        std::unordered_map<int, Frame> lastFrame;
        std::unordered_map<int, uint64_t> staleReuses;

        const auto start = Clock::now();
        const auto deadline = start + runFor;
        auto nextTick = start;
        TimePoint prevTickTime = start;
        int tick = 0;

        while (Clock::now() < deadline) {
            std::this_thread::sleep_until(nextTick);
            TimePoint now = Clock::now();

            double intervalNs = std::chrono::duration<double, std::nano>(now - prevTickTime).count();
            if (tick > 0) tickInterval_.record(intervalNs);
            prevTickTime = now;

            for (auto* surface : surfaces_) {
                int id = surface->id();
                auto fresh = surface->slot().take();
                if (fresh.has_value()) {
                    lastFrame[id] = std::move(*fresh);
                } else {
                    ++staleReuses[id];
                }
                auto it = lastFrame.find(id);
                if (it != lastFrame.end()) {
                    blit(framebuffer, it->second, placements_[id]);
                }
            }

            if (dumpEveryNTicks > 0 && tick % dumpEveryNTicks == 0) {
                dumpPpm(frameDir + "/frame_" + std::to_string(tick) + ".ppm", framebuffer);
            }

            ++tick;
            ++totalTicks_;
            nextTick += period_;
        }

        staleReuses_ = std::move(staleReuses);
    }

    const DurationStats& tickIntervalStats() const { return tickInterval_; }
    uint64_t totalTicks() const { return totalTicks_; }
    uint64_t staleReusesFor(int surfaceId) const {
        auto it = staleReuses_.find(surfaceId);
        return it == staleReuses_.end() ? 0 : it->second;
    }

private:
    void blit(std::vector<uint8_t>& fb, const Frame& f, Placement p) {
        for (int y = 0; y < f.height; ++y) {
            int screenY = p.y + y;
            if (screenY < 0 || screenY >= screenH_) continue;
            for (int x = 0; x < f.width; ++x) {
                int screenX = p.x + x;
                if (screenX < 0 || screenX >= screenW_) continue;
                size_t srcIdx = (static_cast<size_t>(y) * f.width + x) * 3;
                size_t dstIdx = (static_cast<size_t>(screenY) * screenW_ + screenX) * 3;
                fb[dstIdx + 0] = f.rgb[srcIdx + 0];
                fb[dstIdx + 1] = f.rgb[srcIdx + 1];
                fb[dstIdx + 2] = f.rgb[srcIdx + 2];
            }
        }
    }

    void dumpPpm(const std::string& path, const std::vector<uint8_t>& fb) {
        std::ofstream out(path, std::ios::binary);
        if (!out) return;
        out << "P6\n" << screenW_ << " " << screenH_ << "\n255\n";
        out.write(reinterpret_cast<const char*>(fb.data()), fb.size());
    }

    int screenW_, screenH_;
    Clock::duration period_;
    std::vector<SurfaceProducer*> surfaces_;
    std::unordered_map<int, Placement> placements_;
    std::unordered_map<int, uint64_t> staleReuses_;
    DurationStats tickInterval_;
    uint64_t totalTicks_ = 0;
};

} // namespace comp
