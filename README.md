# PagedLOB (`zqr-core`)

**PagedLOB is a C++20 high-update-rate in-memory L3 order book.** Price levels live on 256-tick pages: an 8-bit radix directory plus an occupancy bitmap on each leaf. Orders still use a fixed pool, an intrusive FIFO, and an OrderId hash. The feed is a synthetic MBO stream; the book is differentially verified against a `std::map` reference.

The C++ type is `fastlob::PagedLOB` (alias of `PagedStorageBook`). Namespace, include path, and CMake target stay `fastlob` so existing includes do not break. `FastOrderBook` is the v1 baseline (hash + sorted level list) and is unchanged.

## Architecture

```text
Synthetic MBO  →  Replay (same-thread)  →  Reference / v1 / PagedLOB  →  Shadow Execution Sim  →  Python (pybind11)
```

- **Reference LOB**: `std::map` + `std::list` gold standard
- **FastOrderBook (v1)**: fixed order pool + intrusive FIFO + hash price index + sorted level list
- **PagedLOB**: same order path; price directory is 256 ticks per leaf page
- **Execution Simulator**: shadow limits (never enter the public FIFO) + market VWAP; currently wired to v1
- **SPSC**: not part of MVP

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./scripts/run_benchmark.sh
```

Sanitizers: `-DFASTLOB_SANITIZER=asan` (or `ubsan` / `tsan`).

Python demo (after build; binds v1 `FastOrderBook`):

```bash
PYTHONPATH=build python3 python/demo.py
```

## Layout

```text
include/fastlob/   # types, Order, pool, v1 FastOrderBook, PagedLOB, replay, execution sim
src/               # ScopedAllocProbe (optional global new counter)
tests/             # unit, differential, zero-alloc, execution examples
bench/             # gbench + latency histogram (p50/p99)
python/            # pybind11 module + demo
scripts/           # run_benchmark.sh
```

## Sample results

v1 cancel+re-add, `./scripts/run_benchmark.sh` on Apple M5, Release (synthetic MBO 20k seed=123; 1e6 cancel+re-add, batch=64):

| Engine | p50 | p99 |
| :--- | ---: | ---: |
| Fast cancel+re-add | ~9 ns | ~11 ns |
| Reference cancel+re-add | ~50 ns | ~64 ns |

PagedLOB vs v1 on deep-book churn (cancel best, add worse; GCC 13.3 `-O3`, KVM 4 vCPU Xeon):

| Workload | v1 | PagedLOB |
| :--- | ---: | ---: |
| 1k-level churn p50 | 1.5 µs | 101 ns |
| 10k-level churn p50 | 12.8 µs | 80 ns |

Sparse far-apart prices spend more memory on pages (about 7–11 MB at 2048 distant levels). Re-run the scripts on your machine before citing numbers.

## Limitations

- MVP feed is **synthetic MBO only**
- Single instrument / in-memory only
- Modify: **qty decrease only** (price change = Cancel+Add at the feed layer)
- Shadow orders never mutate the public book
- SPSC lock-free path is out of scope for MVP
- Python still binds v1 `FastOrderBook`, not `PagedLOB`
