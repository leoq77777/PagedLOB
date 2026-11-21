# PagedLOB

**PagedLOB is a C++20 high-update-rate in-memory L3 order book.** The nearest 256 ticks from `best_` sit in a dense hot handle ring; worse prices live on `2^PageBits`-tick cold leaves (default `PageBits=8`, 256 ticks) under an 8-bit radix directory with occupancy bitmaps (`ctz`/`clz`). Orders use a fixed pool, an intrusive FIFO, an open-addressed `OrderIdIndex`, and a 32-bit `Order.level_handle` so cancel/modify/execute/match skip price lookup.

The C++ type is `fastlob::PagedLOB` (alias of `PagedStorageBook`, i.e., `StorageBook<PagedPriceIndex<8, 256>>`). Namespace, include path, and CMake target remain `fastlob`. Full architecture notes, code review, and methodology: [docs/pagedlob-architecture.md](docs/pagedlob-architecture.md).

## Architecture

- **Hot / cold price directory (`PagedPriceIndex<PageBits, HotTicks>`)**:
  - **Hot window**: power-of-two ring of 256 `LevelPool` handles anchored at `best_`; O(1) array lookup on near-touch ticks, O(1) rotation on 1-tick best moves.
  - **Cold radix**: 8-bit radix over the high `(64 - PageBits)` bits of the sign-flipped `int64` tick + `2^PageBits`-tick leaf (`occ[]` bitmap + 32-bit handles). Empty leaves and directory nodes are recycled on internal free lists.
- **Order & level storage (`StorageBook`)**:
  - Pre-sized `OrderPool` (`sizeof(Order) == 56`), `LevelPool` (`sizeof(BookLevel) == 40`, 64-bit `total_qty`), and `OrderIdIndex` (open-addressed, no rehash after construct).
  - `Order.level_handle` lets `cancel`, `modify`, `execute`, and maker fills resolve `BookLevel*` in O(1) without a second price lookup.
- **Separate replay vs matching APIs**:
  - `apply(BookEvent)` (`add` / `cancel` / `modify` / `execute`) never matches, keeping MBO replay event-faithful.
  - `match_limit` / `match_market` walk opposite `best_` and FIFO at maker prices; limit remainders rest, market remainders do not.
- **Verification**: differentially verified against `ReferenceOrderBook` (`std::map` + `std::list`) and `MapStorageBook` across directed edge cases, random streams, page-width variants, hot/cold transitions, and matching walks. `FastOrderBook` (v1, hash + sorted level list) is kept unmodified as an internal benchmark baseline.

## Build & Run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Benchmarks (Release):

```bash
./build/external_compare_bench
./build/price_index_bench
./build/page_param_sweep
```

Sanitizers: `-DFASTLOB_SANITIZER=asan` (or `ubsan` / `tsan`).

## Benchmarks

Measured on **2026-11-19** (`b9430b7`): KVM 4 vCPU Intel Xeon (`model name: Intel(R) Xeon(R) Processor`, Family 6 Model 207), `g++-13` 13.3.0, CMake Release (`-O3 -DNDEBUG`, C++20, no `-march`). Timer: `std::chrono::steady_clock`; reported numbers are **p50 of per-batch means** pooled across 5 reps.

### PagedLOB vs Public Order Books (`external_compare_bench`)

`exist`/`churn`/`sweep`/`sparse` are non-crossing; `match_touch` and `match_walk8` cross via `PagedLOB::match_limit` (and each external book's matching `add`). `v1` has no matching API. Bold = lowest p50 in row.

| Workload | Unit | PagedLOB | v1 | Liquibook | Tzadiko | br-avl |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| `exist_seq` (16k orders, 8 prices) | ns/call | 34.1 | 37.4 | 87.9 | 77.3 | **23.3** |
| `exist_shuffle` (16k orders, 8 prices) | ns/call | 34.1 | 36.6 | 107.9 | 78.8 | **23.5** |
| `churn_1k` (slide best ask, 1k levels) | ns/churn | **98.6** | 1664.8 | 134.1 | 214.0 | 11137.4 |
| `churn_10k` (slide best ask, 10k levels) | ns/churn | **79.3** | 12894.6 | 148.1 | 232.2 | 103251.5 |
| `sweep_in_page` (256 adjacent ticks, top-N) | ns/sweep | 204.0 | **201.9** | 392.8 | 457.2 | 4502.2 |
| `sweep_across` (256-tick stride, top-N) | ns/sweep | 1207.8 | **202.0** | 393.9 | 466.6 | 4506.3 |
| `sparse_jump` (2048 far new-best inserts) | ns/insert | 951.2 | **15.2** | 95.6 | 196.3 | 5906.0 |
| `match_touch` (cross 1 level + restock) | ns/match | 79.5 | — | 66.3 | 113.1 | **52.8** |
| `match_walk8` (cross best, restock far) | ns/match | **26.8** | — | 78.5 | 181.1 | 490.1 |

Selected throughput and `mallinfo2` heap deltas (from the same run):

- `exist_seq`: **28.98 Mops/s**, 1.45 MB heap (vs br-avl 23.3 ns, 1.48 MB)
- `churn_10k`: **12.52 Mops/s**, 1.71 MB heap (vs br-avl 103.3 µs, 1.92 MB)
- `match_walk8`: **35.31 Mops/s**, 23.08 MB heap (pre-sized `1<<18` order pool)

### Structural Memory (`price_index_bench`)

Structural `sizeof` sums (excluding `malloc` rounding; PagedLOB price bytes include free-list reserve):

| Workload | PagedLOB price bytes | PagedLOB total | v1 price bytes | v1 total |
| :--- | ---: | ---: | ---: | ---: |
| `exist_seq` | 6,256 B | 1.45 MB | 2,048 B | 1.45 MB |
| `churn_10k` | 62,080 B | 1.55 MB | 1.05 MB | 2.69 MB |
| `sparse_jump` | 11.11 MB | 11.37 MB | 131 KB | 426 KB |

### Page-Width Sweep (`page_param_sweep`)

Compile-time leaf width `PagedStorageBookT<PageBits>` with the 256-tick hot window enabled (p50):

| ticks | leaf | `exist_seq` | `churn_10k` | `walk_dense` | `walk_1_per_page` | `sparse_jump` |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 160 B | 34.6 ns | 79.2 ns | 176.4 ns | **1038.0 ns** | 1274.6 ns |
| 64 | 288 B | **33.9 ns** | 79.2 ns | 188.1 ns | 1096.8 ns | **1242.3 ns** |
| 128 | 552 B | 34.4 ns | **78.4 ns** | 177.5 ns | 1086.4 ns | 1296.7 ns |
| **256 (default)** | 1080 B | 34.1 ns | 79.2 ns | **162.0 ns** | 1112.5 ns | 1407.2 ns |
| 512 | 2136 B | 34.1 ns | 83.4 ns | 175.9 ns | 1150.2 ns | 1677.4 ns |
| 1024 | 4248 B | 36.4 ns | 78.6 ns | 177.0 ns | 1540.4 ns | 2128.4 ns |

**Trade-offs shown by the numbers**: PagedLOB leads on deep-book churn (`churn_10k` 79.3 ns) and aggressive touch-walking (`match_walk8` 26.8 ns), and matches v1 on near-touch depth scans (`sweep_in_page` 204 ns). Existing-level cancel+add (`exist_seq` 34.1 ns) still trails br-avl (23.3 ns) due to open-addressed `OrderIdIndex` probing/backshift; sparse far-from-touch inserts (`sparse_jump` 951 ns, 11.1 MB index) remain the worst case because each distant best allocates a 7-level radix path. Re-run on your own hardware before citing numbers.

## Layout

```text
include/fastlob/   # PagedLOB (paged_price_index, storage_book, level_pool, price_key), pools, Order, reference & v1 baseline
src/               # ScopedAllocProbe (optional global new counter)
tests/             # unit, differential, matching, hot/cold, page-width, zero-alloc, external agreement
bench/             # external_compare_bench, price_index_bench, page_param_sweep, gbench, latency_hist
docs/              # pagedlob-architecture.md (detailed Chinese design notes, benchmarks, review)
python/            # pybind11 module + demo (currently binds v1 FastOrderBook)
scripts/           # run_benchmark.sh
```

## Limitations

- Single-threaded, single-instrument, in-memory only
- Feed is synthetic MBO only; no self-trade prevention (STP) or hidden/iceberg orders
- `modify` supports qty decrease only (price change = cancel + add)
- Python bindings and `ExecutionSimulator` are not yet wired to `PagedLOB`
