# compositor-sim

A small multithreaded C++ simulation of a display compositor: several
independent "app" threads render frames at their own rate, and a single
vsync-paced compositor thread pulls the freshest frame from each and
blends them into one output image, while a separate thread handles
input events end-to-end. It's a scaled-down model of the problem a real
UI compositing / windowing system has to solve, built to explore that
problem, not to ship a renderer.

## Why this design

Three ideas from real compositing systems show up directly in the code:

**Producers and the compositor never block each other.** Each
`SurfaceProducer` (`include/surface_producer.hpp`) runs on its own
thread at its own target FPS and publishes into a `LatestSlot`
(`include/latest_slot.hpp`) — a depth-1 buffer. The compositor thread
(`include/compositor.hpp`) reads from these slots on a fixed 60Hz
timer. Neither side waits on the other's lock for longer than a
`memcpy`. A stalled compositor tick can't stall rendering, and a
runaway producer can't stall the display.

**Latest-frame-wins, not queue-everything.** If a producer publishes
faster than the compositor consumes (the simulation includes a 90fps
surface against a 60Hz display), the slot overwrites the unconsumed
frame rather than queuing it, and counts it as dropped. Queuing every
frame would only add latency, since the display can never show more
than one buffer's worth of freshness per tick — the same tradeoff a
real compositor's buffer queue makes. Conversely, if a producer is
slower than the display (24fps and 30fps surfaces here), the compositor
reuses ("stale-reuses") the last frame it has rather than blocking,
which is exactly what a display does while waiting on a slow or idle
app.

**Input handling is its own thread, off the render/composite path.**
`EventDispatcher` (`include/event_dispatcher.hpp`) consumes from a
blocking `EventQueue` independent of surface rendering and compositing.
The simulation measures event-to-dispatch latency directly; it stays
sub-millisecond at p99 even while four render threads and a compositor
are all under load, because nothing about input handling depends on
the compositor's tick.

## What it actually does

`src/main.cpp` wires up:

- 4 `SurfaceProducer`s at 24/30/60/90 FPS, each painting a distinct
  color with a sweeping white bar so composited output visibly changes
  frame to frame (useful for eyeballing correctness).
- 1 `Compositor` ticking at 60Hz, placing the 4 surfaces in a 2x2 grid
  and dumping a PPM snapshot every 30 ticks to `frames/`.
- 1 input injector thread generating ~200 simulated events/sec at
  random surfaces, and 1 `EventDispatcher` consuming them.

After a 5-second run it prints:

- Compositor tick interval stats (mean/stddev/min/max/p99) — how close
  the vsync timer holds to its 16.67ms target under thread contention.
- Input event latency stats — time from event injection to dispatch.
- Per-surface produced/dropped/stale-reused frame counts.

Example output:

```
=== compositor-sim summary (5000ms @ 60Hz vsync) ===

Compositor tick interval  n=312    mean=16.667ms  stddev=0.101ms  min=15.536ms  max=17.812ms  p99=16.751ms
Input event latency       n=1000   mean=0.147ms   stddev=1.296ms  min=0.005ms   max=26.434ms  p99=0.242ms
Input events handled: 1000

Surface   TargetFPS   Produced    Dropped   StaleReuse
0         24.000      120         0         193
1         30.000      150         0         163
2         60.000      300         1         14
3         90.000      450         150       13
```

The 90fps surface drops ~1/3 of its frames (exactly what you'd expect
feeding a 60Hz consumer at 1.5x rate); the 24fps and 30fps surfaces get
reused across most ticks since they can't keep up with 60Hz — both
match the intended buffer-queue semantics rather than being a bug.

A sample composited frame (`sample_frame.png`, generated from one of
the PPM dumps) shows the four quadrants with each surface's sweep bar
mid-animation.

## Build & run

Requires CMake 3.16+ and a C++17 compiler.

```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
./run_tests        # unit tests for LatestSlot and EventQueue semantics
./compositor_sim   # runs the 5s simulation, writes frames/*.ppm
```

Convert a dumped frame to view it:

```
convert build/frames/frame_150.ppm sample_frame.png
```

## Project layout

```
include/
  frame.hpp            Frame + InputEvent data types
  latest_slot.hpp       Depth-1 thread-safe buffer with drop tracking
  surface_producer.hpp  Per-surface render thread
  event_queue.hpp        Blocking FIFO for input events
  event_dispatcher.hpp   Input consumer thread + latency measurement
  compositor.hpp          Vsync-paced compositor thread + blending + metrics
src/main.cpp             Wires up the simulation and prints the report
tests/test_latest_slot.cpp  Unit tests (assert-based, no external framework)
```

## Possible extensions

- Replace fixed-position blitting with real alpha compositing
  (currently opaque tiles).
- Model variable/jittery vsync (skipped frames, thermal throttling) to
  see how stale-reuse behaves under a less perfect display clock.
- Swap `LatestSlot`'s mutex for a lock-free SPSC ring to measure the
  effect on tick jitter under contention.
