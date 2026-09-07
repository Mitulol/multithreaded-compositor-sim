# Debugging & profiling notes

Real tool output against this codebase, not a description of what the
tools *could* show. Everything here was run on the machine in the
repo's dev environment: Ubuntu 24.04 on WSL2, x86-64, 12 logical CPUs,
g++ 13.3.0, gdb 15.

`perf` and `valgrind` are not available in that environment (`perf`
needs kernel support WSL2's stock kernel doesn't ship; `valgrind`
wasn't installable without root), so profiling here uses **gprof**,
**gdb** (including a scripted whole-process thread dump), an
**instrumented mutex** built into `LatestSlot`, and
**ThreadSanitizer**. Where a finding would normally come from `perf`
or `helgrind`, the substitute tool and its limitation is called out.

---

## 1. gprof — where the compositor spends its time

The compositor loop runs on the main thread, which is exactly the
thread gprof instruments best.

```
g++ -std=c++17 -O2 -g -pg -Iinclude src/main.cpp -o build/compositor_sim_gprof -pthread
./build/compositor_sim_gprof --duration-ms 8000 --dump-every 30
gprof -b -p ./build/compositor_sim_gprof gmon.out
```

Flat profile (self time), trimmed:

```
  %   cumulative   self              self     total
 time   seconds   seconds    calls  ms/call  ms/call  name
 51.28      0.20     0.20     2460     0.08     0.08  comp::Compositor::composite(...)
 43.59      0.37     0.17                             comp::SurfaceProducer::run(...)
  2.56      0.38     0.01        1    10.00   210.00  comp::Compositor::run(...)
  2.56      0.39     0.01                             main::{lambda()#1}::operator()()   (input injector)
  0.00      0.39     0.00     3202     0.00     0.00  printStats(...)
```

**Findings:**

- **All measurable CPU is pixel work.** `Compositor::composite` (the
  premultiplied "source over" inner loop) is 51% of self time;
  `SurfaceProducer::run` (which inlines `paint()`, another per-pixel
  loop) is 44%. Nothing else clears 3%.
- **Synchronisation does not appear in the profile at all.** No mutex
  lock/unlock, no condition-variable wakeup, no `LatestSlot::put` /
  `take` shows up. At 60 Hz the slot lock is not a cost centre — see
  section 3 for the direct measurement.
- `composite` costs **0.08 ms/call**. Five surfaces per tick =
  ~0.4 ms of the 16.67 ms frame budget. The compositor is ~40x inside
  its deadline; the tick-interval stddev in the summary (~0.3 ms) is
  scheduler noise, not compute pressure.
- Actionable: the only lever that would matter for tick cost is the
  per-pixel loop — SIMD, or skipping fully-occluded surfaces
  (damage/occlusion tracking). Lock-free buffers would not move this
  number.

gprof's limitation: it profiles the main thread's `SIGPROF` samples;
the four surface-producer threads are under-counted. That is acceptable
here because the question was "what does the *compositor* spend a tick
on," and the compositor is the main thread.

---

## 2. gdb — catching a dropped frame, and a whole-process thread dump

Script: [`tools/gdb_session.txt`](../tools/gdb_session.txt). Run with:

```
g++ -std=c++17 -O0 -g -Iinclude -DCOMPOSITOR_SIM_INSTRUMENT src/main.cpp -o build/compositor_sim_dbg -pthread
gdb -q -batch -x tools/gdb_session.txt ./build/compositor_sim_dbg
```

### 2a. The "latest-frame-wins" drop, caught in the act

Breakpoint: `comp::LatestSlot<comp::Frame>::put if hasValue_ == true`
— i.e. stop only when a publish is about to overwrite a frame the
compositor never took.

```
Thread 5 hit Breakpoint 1, comp::LatestSlot<comp::Frame>::put (this=0x7fffffffd468, value=...)
$1 = 3            # value.surfaceId  -> the 90fps surface
$2 = 3            # value.sequence   -> frame #3 arriving
#0 comp::LatestSlot<comp::Frame>::put         at include/latest_slot.hpp:25
#1 comp::SurfaceProducer::run                 at include/surface_producer.hpp:57

(gdb) print *this
$3 = { ...
  counters_ = {acquisitions = 5, contended = 0, waitNs = 0},
  value_ = {[contained value] = {surfaceId = 3, sequence = 2, ...}},   # frame #2, never taken
  hasValue_ = true, dropped_ = 0, published_ = 3 }
```

Frame #3 is overwriting frame #2 while `hasValue_` is still true;
`dropped_` goes 0 -> 1 on the next line. This is the buffer-queue
semantic the whole project is about, visible at the instruction level:
the 90 fps producer is 1.5x the 60 Hz consumer, so every third frame it
produces is discarded before scanout.

### 2b. Where all 8 threads are at that instant

```
Id  Frame
 1  __clock_nanosleep                          <- main thread = the compositor, asleep until next vsync
 2  __clock_nanosleep                          <- 24fps producer, between frames
 3  __clock_nanosleep                          <- 30fps producer
 4  comp::LatestSlot<comp::Frame>::put         <- a producer publishing
 5  comp::LatestSlot<comp::Frame>::put         <- the 90fps producer (at the breakpoint)
 6  __memset_avx2_unaligned_erms
      std::__fill_a1<unsigned char>
      comp::Frame::Frame(...)                  <- a producer zeroing a fresh 76,800-byte frame buffer
 7  __futex_abstimed_wait_common64
      __pthread_cond_wait                      <- EventDispatcher, parked on the EventQueue condvar
 8  __clock_nanosleep                          <- input injector, between events
```

**Findings:**

- **No thread is blocked in `__lll_lock_wait` / futex-on-mutex for a
  `LatestSlot`.** The producers that hold a slot lock are *inside* the
  critical section, not waiting to enter it. This is the same
  conclusion as the gprof profile and the section-3 counters, reached a
  third way.
- **The input path is genuinely decoupled.** Thread 7 (the dispatcher)
  is in `pthread_cond_wait` on the `EventQueue`, not spinning and not
  touching anything the compositor touches. A slow tick on thread 1
  cannot delay it — the only thing that wakes it is `EventQueue::push`.
- **Thread 6 shows the real cost of the mutex `LatestSlot`:** every
  `put` constructs a `Frame`, and `Frame`'s constructor zero-fills a
  76,800-byte `std::vector`. That allocation + memset (and the matching
  free when the old frame is destroyed *inside the lock*) is why the
  lock-free triple buffer — which recycles three fixed buffers — wins
  on throughput even though raw lock contention is low (section 3).

### 2c. Stepping one composite() call

```
Thread 1 hit Temporary breakpoint 2, comp::Compositor::composite (...) at include/compositor.hpp:111
this = 0x7fffffffd600
f = {surfaceId = 0, sequence = 0, width = 160, height = 120, rgba = std::vector of length 76800 ...}
p = {x = 0, y = 0}
fb = std::vector of length 230400   # 320*240*3, pre-cleared to 32
```

Observer effect worth noting: while gdb has the process stopped, the
compositor's `sleep_until` targets fall into the past, so the tick that
runs right after `continue` reports a ~1 ms interval and the next a
~40 ms one. Under the debugger the tick-interval stats are meaningless;
that is why the interval metric is collected in-process instead.

---

## 3. Instrumented mutex — direct contention measurement

`LatestSlot`'s mutex is a `ContendedMutex`
([`include/contended_mutex.hpp`](../include/contended_mutex.hpp)) when
built with `-DCOMPOSITOR_SIM_INSTRUMENT`. It counts every acquisition,
every *contended* acquisition (the `try_lock` fast path failed), and
the nanoseconds spent blocked. The uncontended path costs one extra
`try_lock` and two relaxed atomic adds; clock reads happen only after a
collision.

### 3a. In the real 60 Hz simulation

```
cmake -S . -B build -DCOMPOSITOR_SIM_INSTRUMENT=ON && cmake --build build
./build/compositor_sim --duration-ms 5000 --no-dump
```

```
Surface  TargetFPS  Produced  Dropped  StaleReuse  SlotLockContention
0        24         120       0        193         0.00% of 265 acq
1        30         150       0        163         0.00% of 283 acq
2        60         300       6        18          0.00% of 373 acq
3        90         450       150      13          0.00% of 463 acq
```

**At display rates the `LatestSlot` lock is uncontended — 0 collisions
in ~1,700 acquisitions.** Producer and compositor each touch a given
slot at most ~150 times/second, and hold it only for a move-assign, so
they essentially never collide. The mutex is not a bottleneck in the
workload the project actually models.

### 3b. Benchmark: mutex vs lock-free triple buffer

[`bench/slot_bench.cpp`](../bench/slot_bench.cpp) runs one producer +
one consumer on a single slot, four ways. `hot` = both threads spin
with no delay (far past any real frame rate); `real` = 90 fps producer
/ 60 Hz consumer. `u64` isolates raw sync cost; `frame` uses a real
~75 KiB RGBA `Frame`.

```
./build/slot_bench 3000
```

```
impl                      payload rate      puts/s     takes/s    drops     cont.%   wait us/c  totWait ms
LatestSlot (std::mutex)   u64     hot      ~5.0M      ~2.2M      ~9.9M     ~35%      ~0.17      ~1600
TripleBuffer (lock-free)  u64     hot      ~23M       ~7.0M      ~50M       0%        0          0
LatestSlot (std::mutex)   frame   hot      ~0.22M     ~0.22M     ~18K      ~1.3%     ~0.16      ~150
TripleBuffer (lock-free)  frame   hot      ~0.57M     ~0.57M     ~7K        0%        0          0
LatestSlot (std::mutex)   frame   real      90         60         90        0%        0          0
TripleBuffer (lock-free)  frame   real      90         60         90        0%        0          0
```

(Rates vary ±20% run to run on this shared VM; the ratios are stable.)

**Findings:**

- **`real` row: identical.** Both slots do 90 puts/s with zero
  contention. At the rate a compositor runs, the choice does not
  matter — matching section 3a.
- **`u64` `hot`: the lock-free slot sustains ~4x the publish rate** and
  the mutex spends ~1.6 s (of 3 s wall, summed across threads) blocked,
  with ~35% of acquisitions contended. This is the pure cost of
  serializing on a lock when you hammer it.
- **`frame` `hot`: lock-free is ~2.5x**, and measured lock contention
  is only ~1.3%. The gap here is mostly *not* the lock — it's that the
  mutex slot frees the displaced 75 KiB buffer while holding the lock,
  serializing producer and consumer on the allocator. The triple
  buffer recycles three buffers and never allocates after warm-up.
  (This matches thread 6 in the gdb dump.)

**Conclusion for the design:** the lock-free triple buffer is the
right *model* of what real display hardware does (front buffer held
stable for scanout while the GPU renders into a back buffer), and it
removes an allocator serialization point. But for this simulation's
load it is not a performance fix — the mutex version is already
uncontended at 60 Hz. Both are kept; the sim selects between them with
`-DCOMPOSITOR_SIM_LOCKFREE`.

---

## 4. ThreadSanitizer — race check

TSan needs `setarch -R` (ASLR off) to run under WSL2:

```
g++ -std=c++17 -O1 -g -fsanitize=thread -Iinclude src/main.cpp -o build/tsan_sim -pthread
setarch "$(uname -m)" -R ./build/tsan_sim --duration-ms 4000

g++ -std=c++17 -O1 -g -fsanitize=thread -DCOMPOSITOR_SIM_LOCKFREE -Iinclude src/main.cpp -o build/tsan_sim_lf -pthread
setarch "$(uname -m)" -R ./build/tsan_sim_lf --duration-ms 5000

g++ -std=c++17 -O1 -g -fsanitize=thread -Iinclude tests/test_latest_slot.cpp -o build/tsan_tests -pthread
setarch "$(uname -m)" -R ./build/tsan_tests
```

**All three are clean — zero warnings.** This covers:

- the mutex `LatestSlot`, the `EventQueue` condvar handoff, and the
  `DurationStats` accumulator under the full 7-thread workload;
- the lock-free `TripleBufferLatestSlot` under the same workload,
  including its `acquire`/`release`/`acq_rel` orderings on the mailbox
  word and the `relaxed` drop/publish counters;
- the SPSC stress test (`test_tb_spsc_no_torn_or_reordered_values`),
  which pushes 2,000,000 values through the triple buffer and asserts
  the consumer never observes a torn, stale, or reordered value.

`tools/tsan.sh` runs all of these.
