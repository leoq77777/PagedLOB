# PagedLOB

**PagedLOB is a C++20 high-update-rate, zero-hot-allocation in-memory L3 limit order book (v0.3.0).** The nearest 256 ticks from `best_` sit in a bitmap-indexed hot handle ring (`hot_handle_[256]` + physical occupancy bitmap `hot_occ_[4]`); deeper prices live on `2^PageBits`-tick cold leaves (default `PageBits=8`, 256 ticks) indexed by an 8-bit radix tree for point lookups and linked into a cache-line-packed sorted doubly-linked leaf list for O(1) cold-to-hot promotion and fast cross-page depth sweeps.

The primary C++ type is `fastlob::PagedLOB` (`StorageBook<PagedPriceIndex<8, 256>>`, customizable via `PagedStorageBookT<PageBits, HotTicks>`). Full architecture notes, engineering review closure, and version-by-version changelog: [docs/pagedlob-architecture.md](docs/pagedlob-architecture.md).

## Architecture

- **Two-tier price directory (`PagedPriceIndex<PageBits, HotTicks>`)**:
  - **Hot window**: 256-entry ring of 32-bit `LevelPool` handles anchored at `best_` plus a physical-slot occupancy bitmap `hot_occ_[4]` (`std::countr_zero` / `w &= (w - 1)` via C++20 `<bit>`). Gives O(1) array lookup on near-touch orders, O(1) ring rotation without shifting bitmap words, and zero `memset` on empty-book transitions.
  - **Cold radix + sorted leaf list**: 8-bit radix over the sign-flipped `int64` tick (`DirNode` free-list recycled) down to `2^PageBits`-tick `Leaf` pages whose first 64-byte cache line packs `occ[4]`, `page_key`, `prev_leaf`, `next_leaf`, and `parent`. Gives O(1) `min_leaf_`/`max_leaf_` boundary checks and promotion without climbing directory nodes.
- **Order, level, and ID storage (`StorageBook`)**:
  - Pre-sized `OrderPool` (`sizeof(Order) == 56`), `LevelPool` (`sizeof(BookLevel) == 40`, 64-bit `LevelQty` preventing >4B aggregate overflow), and single-array open-addressed `OrderIdIndex` (`sizeof(Entry) == 16`, single-probe `find_empty_slot`+`commit_slot` and `take`, branchless Knuth Algorithm R backshift).
  - `Order.level_handle` resolves `BookLevel*` in O(1) on `cancel`, `modify`, `execute`, and maker fills without a second price lookup.
- **Separate replay vs matching APIs**:
  - `apply(BookEvent)` (`add` / `cancel` / `modify` / `execute`) never matches, keeping MBO feed replay event-faithful.
  - `match_limit` / `match_market` walk opposite `best_` and FIFO at maker prices; limit remainders rest, market remainders do not.
- **Verification**: 57 unit/differential tests + external differential checks against `ReferenceOrderBook` (`std::map`), `Liquibook`, `Tzadiko`, and `br-avl` (both non-crossing depth and crossing `match_limit` streams), `ScopedAllocProbe` zero-allocation assertions on hot and reserved-cold paths, and ASan + UBSan CI (`.github/workflows/ci.yml`).

## Build & Run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Benchmarks (Release, `-O3 -DNDEBUG`):

```bash
./build/external_compare_bench
./build/price_index_bench
./build/page_param_sweep
./build/fastlob_latency_hist
```

Sanitizers: `-DFASTLOB_SANITIZER=asan+ubsan -DFASTLOB_ENABLE_ALLOC_PROBE=OFF` (or `asan` / `ubsan` / `tsan`).

## Benchmarks (v0.3.0)

Measured on **2026-11-21 (America/New_York, EST)**: KVM 4 vCPU Intel Xeon (`Intel(R) Xeon(R) Processor`, Family 6 Model 207), `g++-13` 13.3.0, CMake Release (`-O3 -DNDEBUG`, C++20, no `-march`). Timer: `std::chrono::steady_clock`; reported numbers are **p50 of per-batch means** pooled across 5 reps.

### PagedLOB vs Public Order Books (`external_compare_bench`)

`exist`/`churn`/`sweep`/`sparse` are non-crossing; `match_touch` and `match_walk8` cross via `PagedLOB::match_limit` (and each external book's matching `add`). `v1` (`FastOrderBook`) has no matching API. Bold = lowest p50 in row.

| Workload | Unit | **PagedLOB v0.3** | v1 (flat) | Liquibook | Tzadiko | br-avl |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| `exist_seq` (16k orders, 8 prices) | ns/call | **20.3** | 24.4 | 88.1 | 77.8 | 22.8 |
| `exist_shuffle` (16k orders, 8 prices) | ns/call | **20.4** | 23.8 | 108.0 | 79.2 | 23.6 |
| `churn_1k` (slide best ask, 1k levels) | ns/churn | **62.1** | 1445.7 | 131.9 | 214.9 | 11228.0 |
| `churn_10k` (slide best ask, 10k levels) | ns/churn | **49.2** | 12904.0 | 148.6 | 235.8 | 107010.9 |
| `sweep_in_page` (256 adjacent ticks, top-N) | ns/sweep | **137.9** | 194.2 | 388.9 | 461.2 | 4482.9 |
| `sweep_across` (256-tick stride, top-N) | ns/sweep | 324.6 | **203.1** | 394.4 | 476.2 | 4489.9 |
| `sparse_jump` (2048 far new-best inserts) | ns/insert | 859.4 | **12.6** | 102.4 | 200.5 | 6067.2 |
| `match_touch` (cross 1 level + restock) | ns/match | **15.2** | — | 66.3 | 120.4 | 57.8 |
| `match_walk8` (cross best, restock far) | ns/match | **23.2** | — | 79.8 | 192.8 | 492.0 |

Selected throughput and `mallinfo2` heap deltas from the same run:

- `exist_seq`: **48.46 Mops/s**, 1.46 MB heap (vs br-avl 22.8 ns, 1.48 MB)
- `churn_10k`: **20.00 Mops/s**, 1.71 MB heap (vs Liquibook 148.6 ns / br-avl 107.0 µs)
- `match_touch`: **49.62 Mops/s**, 15.2 ns p50 (3.80× faster than br-avl, 4.36× faster than Liquibook)
- `match_walk8`: **41.44 Mops/s**, 23.2 ns p50 (3.44× faster than Liquibook, 21.2× faster than br-avl)

### Structural Memory (`price_index_bench`)

| Workload | PagedLOB price bytes | PagedLOB total | `std::map` total | v1 price bytes | v1 total |
| :--- | ---: | ---: | ---: | ---: | ---: |
| `exist_seq` | 6,320 B | 1.45 MB | 1.44 MB | 2,048 B | 1.45 MB |
| `churn_10k` | 63,104 B | 1.55 MB | 1.96 MB | 1.05 MB | 2.69 MB |
| `sparse_jump` | 11.16 MB | 11.42 MB | 360.5 KB | 131 KB | 426 KB |

### Page-Width Sweep (`page_param_sweep`)

Compile-time leaf width `PagedStorageBookT<PageBits>` with the 256-tick hot window enabled (p50):

| ticks | leaf | `exist_seq` | `churn_10k` | `walk_dense` | `walk_1_per_page` | `sparse_jump` |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 184 B | 22.6 ns | 47.9 ns | 114.9 ns | 172.6 ns | 1118.2 ns |
| 64 | 312 B | 20.3 ns | **44.2 ns** | **106.4 ns** | **169.4 ns** | **996.3 ns** |
| 128 | 576 B | 20.0 ns | 46.2 ns | **106.4 ns** | 226.9 ns | 1040.4 ns |
| **256 (default)** | 1104 B | 20.1 ns | 47.5 ns | 106.6 ns | 375.6 ns | 1160.4 ns |
| 512 | 2160 B | 20.2 ns | 48.3 ns | 107.2 ns | 490.6 ns | 1417.9 ns |
| 1024 | 4272 B | **19.3 ns** | 48.6 ns | **106.4 ns** | 706.2 ns | 1868.1 ns |

## Layout

```text
include/fastlob/   # PagedLOB (paged_price_index, storage_book, level_pool, price_key), pools, Order, ExecutionSimulator, reference & v1 baseline
src/               # ScopedAllocProbe (optional global new counter)
tests/             # unit, differential, matching, hot/cold, page-width, zero-alloc, external agreement
bench/             # external_compare_bench, price_index_bench, page_param_sweep, gbench, latency_hist
docs/              # pagedlob-architecture.md (Chinese design notes, benchmarks, review closure & version changelog)
python/            # pybind11 module + demo (binds PagedLOB + ExecutionSimulator)
.github/workflows/ # CI (Release + ScopedAllocProbe, ASan+UBSan)
```

## Limitations

- Single-threaded, single-instrument, in-memory only
- Feed is synthetic MBO only; no self-trade prevention (STP) or hidden/iceberg orders
- `modify` supports qty decrease only (price change = cancel + add)
- Pathological 64-bit sparse far-from-touch jumps (`sparse_jump`) still allocate uncompressed 7-level radix nodes (see [docs/pagedlob-architecture.md](docs/pagedlob-architecture.md) §5 & §8)
