# FastLOB (`zqr-core`)

**FastLOB is a C++20 high-update-rate in-memory state machine**: it consumes synthetic MBO event streams, maintains a differentially verified limit order book, targets zero heap allocations on the add/cancel hot path, and optionally exposes zero-copy snapshots to Python.

## Architecture

```text
Synthetic MBO  →  Replay (same-thread)  →  Reference / Fast LOB  →  Shadow Execution Sim  →  Python (pybind11)
```

- **Reference LOB**: `std::map` + `std::list` gold standard
- **Fast LOB**: fixed order pool + intrusive FIFO + fixed OrderId / price indexes
- **Execution Simulator**: shadow limits (never enter the public FIFO) + market VWAP
- **SPSC**: not part of MVP

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./scripts/run_benchmark.sh
```

Sanitizers: `-DFASTLOB_SANITIZER=asan` (or `ubsan` / `tsan`).

Python demo (after build):

```bash
PYTHONPATH=build python3 python/demo.py
```

## Layout

```text
include/fastlob/   # types, Order, pool, Reference/Fast LOB, replay, execution sim
src/               # ScopedAllocProbe (optional global new counter)
tests/             # unit, differential, zero-alloc, execution examples
bench/             # gbench + latency histogram (p50/p99)
python/            # pybind11 module + demo
scripts/           # run_benchmark.sh
```

## Sample results (Apple M5, Release)

From `./scripts/run_benchmark.sh` (synthetic MBO 20k seed=123; 1e6 cancel+re-add, batch=64):

| Engine | p50 | p99 |
| :--- | ---: | ---: |
| Fast cancel+re-add | ~9 ns | ~11 ns |
| Reference cancel+re-add | ~50 ns | ~64 ns |

Differential test (50k events) and hot-path zero-alloc assertions pass. Re-run the script on your machine before citing numbers.

## Limitations

- MVP feed is **synthetic MBO only**
- Single instrument / in-memory only
- Modify: **qty decrease only** (price change = Cancel+Add at the feed layer)
- Shadow orders never mutate the public book
- SPSC lock-free path is out of scope for MVP
