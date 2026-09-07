#pragma once
#include "frame.hpp"
#include "surface_producer.hpp"
#include "metrics.hpp"
#include <algorithm>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
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
// assigned screen positions using premultiplied "source over", and
// records scheduling metrics.
class Compositor {
public:
    // vsyncJitterMs: peak +/- random perturbation applied to each tick's
    // wake target, modelling a display clock that is not perfectly
    // periodic (scheduling noise, thermal throttling). 0 = ideal clock.
    Compositor(int screenW, int screenH, double vsyncHz, double vsyncJitterMs = 0.0)
        : screenW_(screenW), screenH_(screenH),
          period_(std::chrono::duration_cast<Clock::duration>(
              std::chrono::duration<double>(1.0 / vsyncHz))),
          jitterMs_(vsyncJitterMs) {}

    void addSurface(SurfaceProducer* surface, Placement placement) {
        surfaces_.push_back(surface);
        placements_[surface->id()] = placement;
    }

    void run(std::chrono::milliseconds runFor, int dumpEveryNTicks, const std::string& frameDir) {
        std::vector<uint8_t> framebuffer(static_cast<size_t>(screenW_) * screenH_ * 3, 32);
        std::unordered_map<int, Frame> lastFrame;
        std::unordered_map<int, uint64_t> staleReuses;

        std::mt19937 rng(1234);
        std::uniform_real_distribution<double> jitter(-jitterMs_, jitterMs_);

        const auto start = Clock::now();
        const auto deadline = start + runFor;
        auto nextTick = start;
        TimePoint prevTickTime = start;
        int tick = 0;

        while (Clock::now() < deadline) {
            auto target = nextTick;
            if (jitterMs_ > 0.0) {
                target += std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double, std::milli>(jitter(rng)));
            }
            std::this_thread::sleep_until(target);
            TimePoint now = Clock::now();

            double intervalNs = std::chrono::duration<double, std::nano>(now - prevTickTime).count();
            if (tick > 0) tickInterval_.record(intervalNs);
            prevTickTime = now;

            // Clear to the desktop background, then composite every
            // surface back-to-front. addSurface() order is the z-order.
            std::fill(framebuffer.begin(), framebuffer.end(), static_cast<uint8_t>(32));
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
                    composite(framebuffer, it->second, placements_[id]);
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
    // Premultiplied-alpha "source over": out = src + dst * (1 - src_a).
    // Opaque pixels (src_a == 255) take a plain-copy fast path, which is
    // the common case for a normal app window and is why a compositor
    // tracks per-surface opacity at all.
    void composite(std::vector<uint8_t>& fb, const Frame& f, Placement p) {
        for (int y = 0; y < f.height; ++y) {
            int screenY = p.y + y;
            if (screenY < 0 || screenY >= screenH_) continue;
            for (int x = 0; x < f.width; ++x) {
                int screenX = p.x + x;
                if (screenX < 0 || screenX >= screenW_) continue;
                size_t srcIdx = (static_cast<size_t>(y) * f.width + x) * 4;
                size_t dstIdx = (static_cast<size_t>(screenY) * screenW_ + screenX) * 3;
                uint8_t sr = f.rgba[srcIdx + 0];
                uint8_t sg = f.rgba[srcIdx + 1];
                uint8_t sb = f.rgba[srcIdx + 2];
                uint8_t sa = f.rgba[srcIdx + 3];
                if (sa == 255) {
                    fb[dstIdx + 0] = sr;
                    fb[dstIdx + 1] = sg;
                    fb[dstIdx + 2] = sb;
                } else if (sa != 0) {
                    unsigned inv = 255u - sa;
                    fb[dstIdx + 0] = static_cast<uint8_t>(sr + (fb[dstIdx + 0] * inv + 127) / 255);
                    fb[dstIdx + 1] = static_cast<uint8_t>(sg + (fb[dstIdx + 1] * inv + 127) / 255);
                    fb[dstIdx + 2] = static_cast<uint8_t>(sb + (fb[dstIdx + 2] * inv + 127) / 255);
                }
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
    double jitterMs_;
    std::vector<SurfaceProducer*> surfaces_;
    std::unordered_map<int, Placement> placements_;
    std::unordered_map<int, uint64_t> staleReuses_;
    DurationStats tickInterval_;
    uint64_t totalTicks_ = 0;
};

} // namespace comp
