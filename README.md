# PagedLOB

**PagedLOB is a C++20 high-update-rate, zero-hot-allocation in-memory L3 limit order book (v0.5.0).** Every active price level resides permanently in its aligned `2^PageBits`-tick bitmap `Leaf` page (default `PageBits=8`, 256 ticks; first 64-byte cache line packs `occ[4]`, `page_key`, and sorted doubly-linked `prev_leaf`/`next_leaf`), eliminating hot-cold level migration under wide volatility spikes. A **three-tier zero-migration page cache** (`best_leaf_` + MRU `hot_leaf_` + `near_dir_` covering 256 adjacent pages = 65,536 ticks) resolves near-touch and wide-volatility lookups in $O(1)$, `BookLevel::owner_page` closes depleted levels in strict $O(1)$ without radix walks, and **tagged single-leaf path compression** (`uintptr_t | 1`) avoids intermediate `DirNode` chains on sparse subtrees. Includes a native streaming **NASDAQ TotalView-ITCH 5.0** binary parser (`include/fastlob/itch50.hpp`) and a zero-shadow C ABI adapter (`adapters/matching_engine_benchmark/`) for [`flash1-dev/matching-engine-benchmark`](https://github.com/flash1-dev/matching-engine-benchmark).

The primary C++ type is `fastlob::PagedLOB` (`StorageBook<PagedPriceIndex<8, 256>>`, customizable via `PagedStorageBookT<PageBits>`). Full architecture notes, engineering review closure, and version-by-version changelog: [docs/pagedlob-architecture.md](docs/pagedlob-architecture.md).

## Architecture

- **Zero-migration unified price directory (`PagedPriceIndex<PageBits>`)**:
  - **Aligned bitmap `Leaf` pages**: 256-tick `Leaf` (`sizeof(Leaf) == 1104`), packing `occ[4]`, `page_key`, `prev_leaf`, `next_leaf`, and `parent` in cache line 0 followed by 32-bit `LevelPool` handles (`handle[256]`). Levels never migrate between structures when `best_` jumps or reverts.
  - **Three-tier page cache**: Tier 0 checks `best_leaf_` and MRU `hot_leaf_` (512 ticks across page boundaries in $O(1)$); Tier 1 indexes `near_dir_` (`depth = kLastDirDepth`, covering `256 × 256 = 65,536` consecutive ticks in a single array load); Tier 2 walks an 8-bit sign-flipped radix tree with **tagged single-leaf path compression** (`uintptr_t | 1`) and automatic 1-leaf chain collapse on empty.
- **Order, level, and ID storage (`StorageBook`)**:
  - Pre-sized `OrderPool` (`sizeof(Order) == 56`), `LevelPool` (`sizeof(BookLevel) == 48` with `owner_page` back-pointer + 64-bit `LevelQty`), and single-array open-addressed `OrderIdIndex` (`sizeof(Entry) == 16`, single-probe `find_empty_slot`+`commit_slot` and `take`, branchless Knuth Algorithm R backshift).
  - `Order.level_handle` + `BookLevel.owner_page` resolve `cancel`, `cancel_ex`, `modify`, `execute`, and maker fills in strict $O(1)$ with zero price-index traversal.
- **Separate replay vs zero-allocation matching APIs**:
  - `apply(BookEvent)` (`add` / `cancel` / `modify` / `execute`) never matches, keeping MBO feed replay event-faithful.
  - `match_limit_cb` / `match_ioc_cb` / `match_market_cb` (inline template fill callbacks) and `match_limit` / `match_ioc` / `match_market` walk opposite `best_` and FIFO at maker prices; `cancel_ex` returns `{ok, side, price, qty}` directly from `OrderPool`.
- **Verification**: 65 unit/differential tests + external differential checks against `ReferenceOrderBook` (`std::map`), `Liquibook`, `Tzadiko`, and `br-avl`, **34/34** conformance cases + **50/50** multi-seed audit cells in `matching-engine-benchmark`, `ScopedAllocProbe` zero-allocation assertions, and ASan + UBSan CI (`.github/workflows/ci.yml`).

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
./build/itch_replay_bench /path/to/12302019.NASDAQ_ITCH50.gz --symbols=AAPL,SPY,MSFT
# Build C ABI shared library for flash1-dev/matching-engine-benchmark:
MEB_DIR=/path/to/matching-engine-benchmark ./adapters/matching_engine_benchmark/build.sh
# Or run bare-metal Linux perf_event_open PMU + ITCH benchmarks over SSH:
./scripts/run_remote_perf.sh <ssh-host> [--download-itch]
```

Sanitizers: `-DFASTLOB_SANITIZER=asan+ubsan -DFASTLOB_ENABLE_ALLOC_PROBE=OFF` (or `asan` / `ubsan` / `tsan`).

## Benchmarks (v0.5.0)

Measured on **2026-11-21 (America/New_York, EST)**: KVM 4 vCPU Intel Xeon (`Intel(R) Xeon(R) Processor`, Family 6 Model 207), `g++-13` 13.3.0.

### 1. Public `flash1-dev/matching-engine-benchmark` (5 Regimes, Worst-Case Ranked)

Evaluated via `adapters/matching_engine_benchmark/pagedlob_adapter.cpp` (zero external `g_side`/`g_price`/`g_slot_id` shadow arrays; **34/34** edge-case conformance checks `PASS`, **5/5** canonical SHA-256 report hashes + **960/960** book-state audits `PASS`, **50/50** random seed × scenario cells `PASS`; 5-trial median throughput in **M msgs/s** via `scripts/run_challenge.py --perf-runs 5 --compare`):

| Engine | Structure | `static` | `normal` | `swing-25` | `swing-40` | `flash-crash` | **Worst-Case M/s** | Verdict |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| **PagedLOB v0.5.0** | **Zero-migration paged radix + 16B hash** | **17.12** | **11.38** | **12.03** | **10.34** | 10.70 | **10.34** (`swing-40`) | **VALID** |
| `lcv3` (LC++ v3) | Treap + external slot/price arrays | 16.87 | 8.64 | 9.88 | 9.57 | **10.75** | 8.64 (`normal`) | VALID |
| `cpp_orderbook` | Red-black tree + hash | 6.45 | 6.75 | 5.71 | 5.94 | 6.34 | 5.71 (`swing-25`) | VALID |
| `cpptrader` | Intrusive AVL + hash | 5.33 | 5.48 | 4.96 | 5.40 | 5.23 | 4.96 (`swing-25`) | VALID |
| `kautenja` | Unbalanced BST + map | 3.61 | 5.15 | 5.99 | 5.57 | 5.56 | 3.61 (`static`) | VALID |
| `brprojects` (`br-avl`) | Doubly-linked AVL + `unordered_map` | 5.98 | 0.50 | 0.16 | 0.23 | 0.56 | 0.16 (`swing-25`) | VALID |

### 2. Real Market Replay — NASDAQ TotalView-ITCH 5.0 (`itch_replay_bench`)

Full-day replay of official **`12302019.NASDAQ_ITCH50.gz`** (`268,744,780` wire messages streamed at `8.60 M wire msgs/s`; `5,129,390` normalized L3 `BookEvent`s across `AAPL`, `SPY`, and `MSFT`, verified 100% differentially across `PagedLOB`, `MapStorageBook`, `FastOrderBook`, and `ReferenceOrderBook` at 100 intraday checkpoints + EOD; `taskset -c 0`, `batch=128`, 1 warmup + 5 timed reps):

| Symbol | Book | Full-Day Events | 256-Tick Near-Touch % | Mops/s | p50 (ns) | p99 (ns) | p99.9 (ns) | 6.5h Day Replay |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **SPY** | **PagedLOB v0.5** | 2,249,458 | **91.48%** | **62.67** | **15.1** | **23.1** | **31.6** | **35.9 ms** |
| SPY | `v1` (flat) | 2,249,458 | — | 42.23 | 23.3 | 40.6 | 95.8 | 53.3 ms |
| SPY | `std::map` | 2,249,458 | — | 38.47 | 25.0 | 41.5 | 94.9 | 58.5 ms |
| SPY | `ref_map_list` | 2,249,458 | — | 15.15 | 64.6 | 89.0 | 150.8 | 148.5 ms |
| **MSFT** | **PagedLOB v0.5** | 1,270,785 | **88.28%** | **52.02** | **18.5** | **26.5** | **57.4** | **24.4 ms** |
| MSFT | `std::map` | 1,270,785 | — | 29.20 | 33.0 | 55.0 | 112.7 | 43.5 ms |
| MSFT | `v1` (flat) | 1,270,785 | — | 20.43 | 35.9 | 177.3 | 1981.7 | 62.2 ms |
| **AAPL** | **PagedLOB v0.5** | 1,609,147 | **75.08%** | **43.95** | **21.9** | **30.6** | **87.9** | **36.6 ms** |
| AAPL | `std::map` | 1,609,147 | — | 21.99 | 44.2 | 65.9 | 122.3 | 73.2 ms |
| AAPL | `ref_map_list` | 1,609,147 | — | 11.28 | 86.5 | 126.1 | 186.4 | 142.7 ms |
| AAPL | `v1` (flat) | 1,609,147 | — | 7.38 | 120.5 | 336.6 | 2406.7 | 218.0 ms |

### 3. PagedLOB vs Public Order Books (`external_compare_bench`)

`exist`/`churn`/`sweep`/`sparse` are non-crossing; `match_touch` and `match_walk8` cross via `PagedLOB::match_limit`. Bold = lowest p50 in row.

| Workload | Unit | **PagedLOB v0.5** | v1 (flat) | Liquibook | Tzadiko | br-avl |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| `exist_seq` (16k orders, 8 prices) | ns/call | **20.9** | 26.7 | 89.2 | 85.7 | 24.5 |
| `exist_shuffle` (16k orders, 8 prices) | ns/call | **20.4** | 26.3 | 107.6 | 90.6 | 24.2 |
| `churn_1k` (slide best ask, 1k levels) | ns/churn | **48.7** | 1507.0 | 138.4 | 215.8 | 10718.0 |
| `churn_10k` (slide best ask, 10k levels) | ns/churn | **38.5** | 12931.3 | 149.2 | 235.8 | 100568.0 |
| `sweep_in_page` (256 adjacent ticks, top-N) | ns/sweep | **109.1*** | 203.6 | 394.4 | 489.5 | 4694.8 |
| `sweep_across` (256-tick stride, top-N) | ns/sweep | 470.2* | **203.5** | 383.4 | 484.8 | 4693.3 |
| `sparse_jump` (2048 far new-best inserts) | ns/insert | 252.0 | **13.2** | 118.7 | 194.1 | 5622.5 |
| `match_touch` (cross 1 level + restock) | ns/match | **23.6** | — | 68.3 | 124.3 | 54.0 |
| `match_walk8` (cross best, restock far) | ns/match | **21.1** | — | 79.9 | 182.3 | 466.9 |

*\*Pure `price_index_bench` `sweep_in_page` = **109.1 ns** (`9.03 Mops/s`); `external_compare_bench` wraps snapshots into generic vectors (`291.8 ns` vs `394.4 ns` Liquibook).*

### 4. Structural Memory (`price_index_bench`)

| Workload | PagedLOB v0.5 price bytes | PagedLOB total | `std::map` total | v1 price bytes | v1 total |
| :--- | ---: | ---: | ---: | ---: | ---: |
| `exist_seq` | 8,624 B | 1.45 MB | 1.44 MB | 2,048 B | 1.45 MB |
| `churn_10k` | **64,304 B** | **1.63 MB** | 2.04 MB | 1.05 MB | 2.69 MB |
| `sparse_jump` | **2.54 MB** (down `4.4×` from `11.16 MB`) | **2.82 MB** | 376.9 KB | 131 KB | 426 KB |

## Layout

```text
include/fastlob/                    # PagedLOB (paged_price_index, storage_book, level_pool, price_key, itch50), pools, Order, ExecutionSimulator
adapters/matching_engine_benchmark/ # Zero-shadow C ABI adapter + build.sh for flash1-dev/matching-engine-benchmark
src/                                # ScopedAllocProbe (optional global new counter)
tests/                              # 65 unit, differential, matching, IOC/callback, zero-migration spike, ITCH 5.0 & external tests
bench/                              # external_compare_bench, price_index_bench, page_param_sweep, itch_replay_bench, perf_counters
docs/                               # pagedlob-architecture.md (Chinese design notes, benchmarks, review closure & version changelog)
python/                             # pybind11 module + demo (binds PagedLOB + ExecutionSimulator)
.github/workflows/                  # CI (Release + ScopedAllocProbe, ASan+UBSan)
```

## Limitations

- Single-threaded, single-instrument per book instance, in-memory only
- No self-trade prevention (STP) or hidden/iceberg orders
- `apply()`/`modify()` supports qty decrease in-place; price/qty-up replacement uses `cancel` + `add` (matching ITCH 5.0 `'X'` vs `'U'` and `matching-engine-benchmark` `engine_on_modify`)
