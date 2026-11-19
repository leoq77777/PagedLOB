# PagedLOB (`zqr-core`)

**PagedLOB is a C++20 high-update-rate in-memory L3 order book.** Price levels live on 256-tick pages: an 8-bit radix directory plus an occupancy bitmap on each leaf. Orders still use a fixed pool, an intrusive FIFO, and an OrderId hash. The feed is a synthetic MBO stream; the book is differentially verified against a `std::map` reference.

The C++ type is `fastlob::PagedLOB` (alias of `PagedStorageBook`). Namespace, include path, and CMake target stay `fastlob` so existing includes do not break. `FastOrderBook` is the v1 baseline (hash + sorted level list) and is unchanged.

## Architecture

```text
Synthetic MBO  →  Replay (same-thread)  →  Reference / v1 / PagedLOB  →  Shadow Execution Sim  →  Python (pybind11)
```

- **Reference LOB**: `std::map` + `std::list` gold standard
- **FastOrderBook (v1)**: fixed order pool + intrusive FIFO + hash price index + sorted level list
- **PagedLOB**: same order path; price directory is `2^PageBits` ticks per leaf (default `PageBits=8`, 256 ticks). The nearest 256 ticks from best sit in a dense hot window; worse prices stay in the radix. `match_limit` / `match_market` walk `best_` and the FIFO; `apply()` still does not match (MBO replay).
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

Page width is a compile-time parameter (`PagedStorageBookT<PageBits>`). Default stays 256. `page_param_sweep` on GCC 13.3 `-O3`, KVM Xeon (same machine as the churn table):

| ticks | leaf | churn 10k p50 | dense walk 128 lvl | 1-per-page walk | sparse insert | sparse bytes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 160 B | 74 ns | 237 ns | 923 ns | 1085 ns | 9.2 MB |
| 64 | 288 B | 73 ns | 245 ns | 914 ns | 1089 ns | 9.5 MB |
| 128 | 552 B | 78 ns | 456 ns | 963 ns | 1141 ns | 10.0 MB |
| **256** | 1080 B | 76 ns | 537 ns | 962 ns | 1242 ns | 11.1 MB |
| 512 | 2136 B | 78 ns | 545 ns | 1001 ns | 1479 ns | 13.3 MB |
| 1024 | 4248 B | 78 ns | 548 ns | 1402 ns | 1898 ns | 17.6 MB |

Churn is flat across widths. Dense `top_n` and sparse inserts prefer 32–64 tick leaves. Existing-level updates stay ~37 ns at every width. Re-run the scripts on your machine before citing numbers.

## Limitations

- MVP feed is **synthetic MBO only**
- Single instrument / in-memory only
- Modify: **qty decrease only** (price change = Cancel+Add at the feed layer)
- Shadow orders never mutate the public book
- SPSC lock-free path is out of scope for MVP
- Python still binds v1 `FastOrderBook`, not `PagedLOB`
