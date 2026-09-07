# compositor-sim

A small multithreaded C++ simulation of a display compositor: several
independent "app" threads render frames at their own rate, and a single
vsync-paced compositor thread pulls the freshest frame from each and
blends them into one output image, while a separate thread handles
input events end-to-end. It's a scaled-down model of the problem a real
UI compositing / windowing system has to solve, built to explore that
problem, not to ship a renderer.

## Why this design

Four ideas from real compositing systems show up directly in the code:

**Producers and the compositor never block each other.** Each
`SurfaceProducer` (`include/surface_producer.hpp`) runs on its own
thread at its own target FPS and publishes into a depth-1 buffer slot
(`FrameSlot`, `include/frame_slot.hpp`). The compositor thread
(`include/compositor.hpp`) reads from these slots on a fixed 60Hz
timer. Neither side waits on the other's lock for longer than a move.
A stalled compositor tick can't stall rendering, and a runaway producer
can't stall the display.

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

**Two implementations of that slot: mutex and lock-free.** The default
`LatestSlot` (`include/latest_slot.hpp`) guards a single value with a
`std::mutex`. `TripleBufferLatestSlot` (`include/triple_buffer_slot.hpp`)
is a lock-free single-producer/single-consumer alternative with the
identical contract: three frame buffers and one atomic "mailbox" word
naming the most-recently-published one. The producer `exchange()`s its
index into the mailbox and recycles whatever buffer was there
(wait-free); the consumer CAS-swaps its buffer in for the published one
(lock-free). This is the scheme real display pipelines use so the GPU
can keep rendering into a back buffer while scanout holds the front
buffer stable — and it removes the per-frame allocate/free that the
mutex version does inside its critical section. Build with
`-DCOMPOSITOR_SIM_LOCKFREE` to run the whole simulation on it;
`bench/slot_bench.cpp` measures the difference (see
[Debugging & profiling](#debugging--profiling)). Short version: at
60Hz the mutex is completely uncontended and the choice doesn't
matter; only when the slot is hammered far past display rates does the
lock-free version pull ahead (~2.5–4x publish throughput).

**Input handling is its own thread, off the render/composite path.**
`EventDispatcher` (`include/event_dispatcher.hpp`) consumes from a
blocking `EventQueue` independent of surface rendering and compositing.
The simulation measures event-to-dispatch latency directly; it stays
sub-millisecond at p99 even while five render threads and a compositor
are all under load, because nothing about input handling depends on
the compositor's tick. A gdb thread dump (see the profiling notes)
shows the dispatcher parked in `pthread_cond_wait` on the queue —
never polling, never touching compositor state.

## Compositing: premultiplied alpha

The compositor does real "source over" blending, not opaque tile
blitting. Frames are stored as premultiplied-alpha RGBA
(`include/frame.hpp`); the compositor's inner loop
(`Compositor::composite`) computes `out = src + dst * (1 - src_a)` per
channel, with a plain-copy fast path for fully opaque pixels (the
common case for a normal window, and the reason a compositor tracks
per-surface opacity at all).

The simulation stacks a fifth, translucent surface (a 45%-opacity
"notification" band) over the four-way grid so the composited output
shows actual blending — `sample_frame.png` is a dump mid-animation,
with the overlay lightening all four quadrants and each surface's
opaque white sweep bar showing through at full strength.

## What it actually does

`src/main.cpp` wires up:

- 4 opaque `SurfaceProducer`s at 24/30/60/90 FPS, each painting a
  distinct color with a sweeping white bar so composited output visibly
  changes frame to frame (useful for eyeballing correctness).
- 1 translucent overlay `SurfaceProducer` (id 4, 45% opacity, 30 FPS)
  spanning the middle third of the screen.
- 1 `Compositor` ticking at 60Hz, placing the grid surfaces in a 2x2
  layout with the overlay composited last (on top), dumping a PPM
  snapshot every N ticks to `frames/`.
- 1 input injector thread generating ~200 simulated events/sec at
  random surfaces, and 1 `EventDispatcher` consuming them.

CLI flags: `--duration-ms N` (default 5000), `--vsync-hz H` (60),
`--vsync-jitter-ms J` (0 — peak ± perturbation of each tick's wake
target, to model an imperfect display clock), `--dump-every N` (30),
`--no-dump`.

After the run it prints:

- Compositor tick interval stats (mean/stddev/min/max/p99) — how close
  the vsync timer holds to its 16.67ms target under thread contention.
- Input event latency stats — time from event injection to dispatch.
- Per-surface produced/dropped/stale-reused frame counts, plus
  measured lock contention on that surface's slot when built with
  `-DCOMPOSITOR_SIM_INSTRUMENT`.

Example output (`-DCOMPOSITOR_SIM_INSTRUMENT=ON`, mutex slot):

```
=== compositor-sim summary (5000ms @ 60Hz vsync) ===
frame slot impl: LatestSlot (std::mutex)

Compositor tick interval    n=   312  mean= 16.669ms  stddev=  0.271ms  min= 14.950ms  max= 17.995ms  p99= 17.232ms
Input event latency         n=  1000  mean=  0.161ms  stddev=  0.068ms  min=  0.043ms  max=  0.704ms  p99=  0.406ms
Input events handled: 1000

Surface   TargetFPS   Produced    Dropped   StaleReuse  SlotLockContention
0         24          120         0         193         0.00% of 433 acq
1         30          150         0         163         0.00% of 463 acq
2         60          300         1         14          0.00% of 613 acq
3         90          450         150       13          0.00% of 763 acq
4         30          150         0         163         0.00% of 463 acq
```

The 90fps surface drops ~1/3 of its frames (exactly what you'd expect
feeding a 60Hz consumer at 1.5x rate); the 24fps and 30fps surfaces get
reused across most ticks since they can't keep up with 60Hz — both
match the intended buffer-queue semantics rather than being a bug. The
60fps surface sits near the display rate, so depending on scheduling
phase it lands cleanly or beats against the tick and drops a handful of
frames per run. Slot lock contention is measured at zero: producer and
compositor touch a given slot only ~150 times/second combined and hold
it only for a move.

## Build & run

Requires CMake 3.16+ and a C++17 compiler.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/run_tests        # LatestSlot, TripleBufferLatestSlot, EventQueue semantics + SPSC stress
./build/compositor_sim   # the 5s simulation, writes frames/*.ppm
./build/slot_bench       # mutex vs lock-free slot contention benchmark
```

Options:

```
cmake -S . -B build -DCOMPOSITOR_SIM_LOCKFREE=ON     # run the sim on the lock-free triple buffer
cmake -S . -B build -DCOMPOSITOR_SIM_INSTRUMENT=ON   # measure LatestSlot lock contention
```

Convert a dumped frame to view it:

```
convert build/frames/frame_120.ppm sample_frame.png
```

## Debugging & profiling

[`docs/profiling.md`](docs/profiling.md) has the real tool output —
this project was profiled, not just written. Summary of what was found:

- **gprof**: the compositor tick is 100% pixel work —
  `Compositor::composite` is 51% of self time, `paint()` in the
  producers another 44%, synchronization doesn't appear at all.
  `composite` costs 0.08 ms/call, ~0.4 ms/tick for five surfaces
  against a 16.67 ms budget.
- **gdb** ([`tools/gdb_session.txt`](tools/gdb_session.txt)): a
  scripted session catches the latest-frame-wins drop mid-`put()`
  (the 90fps surface overwriting an untaken frame), then dumps all 8
  threads — compositor asleep until vsync, producers between frames or
  inside a `put`, one producer memset-ing a fresh 76 KB frame buffer,
  the input dispatcher parked on the event-queue condvar, and **nobody
  blocked on a slot mutex**.
- **Instrumented mutex** (`include/contended_mutex.hpp`): 0 contended
  acquisitions out of ~1,700 in a real 5s run. In `slot_bench`'s
  spin-loop stress, ~35% of acquisitions contend and the lock-free
  triple buffer sustains ~2.5–4x the publish rate — but most of that
  gap is the mutex version freeing the displaced frame buffer inside
  the lock, not the lock itself.
- **ThreadSanitizer** ([`tools/tsan.sh`](tools/tsan.sh)): clean on the
  mutex build, the lock-free build, and a 2M-iteration SPSC stress
  test of the triple buffer.

(`perf` and `valgrind` aren't available in the WSL2 dev environment;
`docs/profiling.md` notes where each substitute tool has a blind spot.)

## Project layout

```
include/
  frame.hpp              Frame (premultiplied RGBA) + InputEvent data types
  contended_mutex.hpp    std::mutex wrapper that records lock contention
  latest_slot.hpp        Depth-1 mutex-based slot with drop tracking
  triple_buffer_slot.hpp Lock-free SPSC triple-buffer slot, same contract
  frame_slot.hpp         Selects one of the two at build time
  surface_producer.hpp   Per-surface render thread (opaque or translucent)
  event_queue.hpp        Blocking FIFO for input events
  event_dispatcher.hpp   Input consumer thread + latency measurement
  compositor.hpp         Vsync-paced compositor thread + premultiplied blend + metrics
  metrics.hpp            Thread-safe duration accumulator (mean/stddev/p99)
src/main.cpp             Wires up the simulation, parses flags, prints the report
bench/slot_bench.cpp     Mutex vs lock-free slot contention benchmark
tests/test_latest_slot.cpp  Unit + SPSC stress tests (custom CHECK macro, assert-safe under NDEBUG)
tools/gdb_session.txt    Scripted gdb inspection session
tools/tsan.sh            Build + run everything under ThreadSanitizer
docs/profiling.md        Real gprof / gdb / TSan / contention findings
```

## Possible extensions

- Damage / occlusion tracking: skip compositing surfaces that are
  fully hidden, and only re-blend changed screen regions instead of
  clearing and redrawing the whole framebuffer each tick.
- SIMD the `composite` inner loop (it's the measured hot path).
- Model a GPU-fence-style handoff: a producer signals "frame N ready"
  and the compositor waits on the fence with a timeout rather than
  polling the slot each tick.
- Per-surface transforms (scale/rotation) so `composite` does real
  sampling rather than a 1:1 copy.
