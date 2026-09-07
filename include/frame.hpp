#pragma once
#include <cstdint>
#include <vector>
#include <chrono>

namespace comp {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// A single rendered frame from one surface (app/window). Mirrors the
// role of a GraphicBuffer in a real compositing system: a fixed-size
// pixel payload plus the metadata the compositor needs to schedule and
// place it, but with no ties to a specific GPU/display API.
//
// Pixels are stored premultiplied-alpha RGBA, row-major. Premultiplied
// is what real compositors carry end to end: it makes the "source over"
// blend a single multiply-add per channel and makes compositing
// associative, so a stack of surfaces can be combined in any grouping.
struct Frame {
    int surfaceId = -1;
    uint64_t sequence = 0;          // monotonically increasing per-surface
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;      // width*height*4, premultiplied, row-major
    TimePoint producedAt{};

    Frame() = default;
    Frame(int id, uint64_t seq, int w, int h)
        : surfaceId(id), sequence(seq), width(w), height(h),
          rgba(static_cast<size_t>(w) * h * 4, 0),
          producedAt(Clock::now()) {}
};

struct InputEvent {
    int surfaceId;
    int type;              // 0 = tap/click, 1 = drag, 2 = key
    TimePoint injectedAt;
};

} // namespace comp
