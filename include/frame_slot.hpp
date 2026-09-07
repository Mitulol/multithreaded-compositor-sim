#pragma once
#include "frame.hpp"
#include "latest_slot.hpp"
#include "triple_buffer_slot.hpp"

namespace comp {

// The simulation's surface producers and compositor talk to their
// inter-thread buffer only through this alias. Build with
// -DCOMPOSITOR_SIM_LOCKFREE to swap the mutex-based LatestSlot for the
// lock-free triple buffer without touching any other code, so the two
// concurrency strategies can be compared under an identical workload.
#ifdef COMPOSITOR_SIM_LOCKFREE
template <typename T> using FrameSlotT = TripleBufferLatestSlot<T>;
inline constexpr const char* kFrameSlotImpl = "TripleBufferLatestSlot (lock-free)";
#else
template <typename T> using FrameSlotT = LatestSlot<T>;
inline constexpr const char* kFrameSlotImpl = "LatestSlot (std::mutex)";
#endif

using FrameSlot = FrameSlotT<Frame>;

} // namespace comp
