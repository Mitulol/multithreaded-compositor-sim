#!/usr/bin/env bash
# Build and run everything under ThreadSanitizer.
# WSL2 needs `setarch -R` (ASLR off) or TSan aborts with an
# "unexpected memory mapping" error before main().
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-g++}
FLAGS="-std=c++17 -O1 -g -fsanitize=thread -Iinclude -pthread"
mkdir -p build

run() { echo "== $* =="; setarch "$(uname -m)" -R "$@"; echo; }

$CXX $FLAGS tests/test_latest_slot.cpp -o build/tsan_tests
$CXX $FLAGS src/main.cpp -o build/tsan_sim
$CXX $FLAGS -DCOMPOSITOR_SIM_LOCKFREE src/main.cpp -o build/tsan_sim_lf

run ./build/tsan_tests
run ./build/tsan_sim --duration-ms 4000 --no-dump
run ./build/tsan_sim_lf --duration-ms 4000 --no-dump

echo "ThreadSanitizer: no warnings above => clean"
