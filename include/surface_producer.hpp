#pragma once
#include "frame.hpp"
#include "frame_slot.hpp"
#include <atomic>
#include <thread>
#include <array>
#include <cstdlib>

namespace comp {

struct Color { uint8_t r, g, b; };

// Simulates one client application/window rendering at its own target
// frame rate on its own thread, independent of the display's refresh
// rate. Each surface owns a LatestSlot it publishes into; it never
// blocks on the compositor, which is the point: a slow or stalled
// compositor thread must not be able to stall a producer thread, and
// vice versa.
//
// `alpha` is the surface's constant opacity in [0,1]. An opaque surface
// (alpha == 1) lets the compositor take a plain-copy fast path; a
// translucent one is blended "source over" whatever is already in the
// framebuffer.
class SurfaceProducer {
public:
    SurfaceProducer(int id, int width, int height, double targetFps, Color color,
                    double alpha = 1.0)
        : id_(id), width_(width), height_(height),
          period_(std::chrono::duration_cast<Clock::duration>(
              std::chrono::duration<double>(1.0 / targetFps))),
          color_(color),
          alpha_(alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha)) {}

    FrameSlot& slot() { return slot_; }
    int id() const { return id_; }

    void start(std::chrono::milliseconds runFor) {
        running_ = true;
        thread_ = std::thread([this, runFor] { run(runFor); });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    void run(std::chrono::milliseconds runFor) {
        const auto start = Clock::now();
        const auto deadline = start + runFor;
        auto nextTick = start;
        uint64_t seq = 0;

        while (running_ && Clock::now() < deadline) {
            Frame f(id_, seq++, width_, height_);
            paint(f, seq);
            slot_.put(std::move(f));

            nextTick += period_;
            std::this_thread::sleep_until(nextTick);
        }
    }

    // Paints a simple animated pattern (a sweeping vertical bar) so
    // composited output visibly differs frame to frame -- useful for
    // sanity-checking correctness from the dumped PPM snapshots. Output
    // is premultiplied RGBA: the sweep bar is fully opaque, the rest of
    // the surface carries the surface's constant alpha.
    void paint(Frame& f, uint64_t seq) {
        int sweepX = static_cast<int>(seq * 4) % f.width;
        const int barA = 255;
        const int bodyA = static_cast<int>(alpha_ * 255.0 + 0.5);
        for (int y = 0; y < f.height; ++y) {
            for (int x = 0; x < f.width; ++x) {
                size_t idx = (static_cast<size_t>(y) * f.width + x) * 4;
                bool onBar = std::abs(x - sweepX) < 6;
                int a = onBar ? barA : bodyA;
                int r = onBar ? 255 : color_.r;
                int g = onBar ? 255 : color_.g;
                int b = onBar ? 255 : color_.b;
                // premultiply
                f.rgba[idx + 0] = static_cast<uint8_t>(r * a / 255);
                f.rgba[idx + 1] = static_cast<uint8_t>(g * a / 255);
                f.rgba[idx + 2] = static_cast<uint8_t>(b * a / 255);
                f.rgba[idx + 3] = static_cast<uint8_t>(a);
            }
        }
    }

    int id_;
    int width_, height_;
    Clock::duration period_;
    Color color_;
    double alpha_;
    FrameSlot slot_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace comp
