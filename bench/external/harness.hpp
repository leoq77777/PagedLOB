#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <malloc.h>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

// Shared timing harness. A book exposes:
//   Book(size_t max_orders, size_t max_levels)
//   bool add(uint64_t id, bool buy, int64_t price, uint32_t qty)
//   bool cancel(uint64_t id)
//   uint64_t sweep_bids()
// Percentiles are of per-batch means. Prices stay in 1..2e9 so int32 books
// and Liquibook (price 0 means market) can all represent them.

struct BenchRow {
    std::string workload;
    std::string book;
    std::string unit;
    int batch{0};
    int samples{0};
    double mops{0};
    double p50{0};
    double p99{0};
    double p999{0};
    double p50_sd{0};
    std::uint64_t heap_bytes{0};
};

std::vector<BenchRow>& bench_rows();
bool& bench_failed();
volatile std::uint64_t& bench_sink();

inline double bench_percentile(std::vector<double> samples, double p) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double idx = p * static_cast<double>(samples.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, samples.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return samples[lo] * (1.0 - frac) + samples[hi] * frac;
}

inline double bench_stdev(const std::vector<double>& values) {
    if (values.size() < 2) {
        return 0;
    }
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    double acc = 0;
    for (double value : values) {
        const double d = value - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(values.size() - 1));
}

inline std::uint64_t bench_now_ns() {
    using clock_type = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count());
}

inline std::uint64_t bench_heap_bytes() {
    const struct mallinfo2 info = mallinfo2();
    // uordblks is the brk heap. Allocations above the mmap threshold sit in hblkhd.
    return static_cast<std::uint64_t>(info.uordblks) + static_cast<std::uint64_t>(info.hblkhd);
}

template <typename Book, typename Setup, typename Batch>
void bench_measure(const char* workload, const char* book_name, const char* unit, int reps, int batches,
                   int ops_per_batch, std::size_t cap_orders, std::size_t cap_levels, bool memory_after_work,
                   Setup&& setup, Batch&& batch) {
    std::vector<double> pooled;
    std::vector<double> rep_means;
    pooled.reserve(static_cast<std::size_t>(reps * batches));
    std::uint64_t heap = 0;
    double seconds = 0;
    std::uint64_t ops = 0;
    for (int rep = 0; rep < reps; ++rep) {
        const std::uint64_t before = bench_heap_bytes();
        Book book(cap_orders, cap_levels);
        if (!setup(book)) {
            std::cerr << "setup failed " << workload << " " << book_name << "\n";
            bench_failed() = true;
            return;
        }
        if (rep == 0 && !memory_after_work) {
            heap = bench_heap_bytes() - before;
        }
        for (int i = 0; i < batches; ++i) {
            const std::uint64_t t0 = bench_now_ns();
            batch(book);
            const std::uint64_t t1 = bench_now_ns();
            pooled.push_back(static_cast<double>(t1 - t0) / static_cast<double>(ops_per_batch));
            seconds += static_cast<double>(t1 - t0) * 1e-9;
            ops += static_cast<std::uint64_t>(ops_per_batch);
        }
        if (rep == 0 && memory_after_work) {
            heap = bench_heap_bytes() - before;
        }
        double sum = 0;
        for (int i = 0; i < batches; ++i) {
            sum += pooled[pooled.size() - static_cast<std::size_t>(batches - i)];
        }
        rep_means.push_back(sum / static_cast<double>(batches));
    }
    BenchRow row;
    row.workload = workload;
    row.book = book_name;
    row.unit = unit;
    row.batch = ops_per_batch;
    row.samples = static_cast<int>(pooled.size());
    row.mops = seconds > 0 ? (static_cast<double>(ops) / seconds) / 1e6 : 0;
    row.p50 = bench_percentile(pooled, 0.50);
    row.p99 = bench_percentile(pooled, 0.99);
    row.p999 = bench_percentile(pooled, 0.999);
    row.p50_sd = bench_stdev(rep_means);
    row.heap_bytes = heap;
    bench_rows().push_back(row);
}

template <typename Book>
bool bench_build_existing(Book& book, const std::vector<std::pair<std::uint64_t, std::int64_t>>& ids) {
    for (const auto& [id, price] : ids) {
        if (!book.add(id, true, price, 1)) {
            return false;
        }
    }
    for (const auto& [id, price] : ids) {
        if (!book.cancel(id) || !book.add(id, true, price, 1)) {
            return false;
        }
    }
    return true;
}

template <typename Book>
void bench_existing(const char* name, bool shuffled) {
    constexpr int kOrders = 16384;
    constexpr int kPrices = 8;
    std::vector<std::pair<std::uint64_t, std::int64_t>> ids;
    ids.reserve(kOrders);
    for (int i = 0; i < kOrders; ++i) {
        ids.emplace_back(static_cast<std::uint64_t>(i + 1), static_cast<std::int64_t>(1 + (i % kPrices)));
    }
    if (shuffled) {
        std::mt19937 rng(12345);
        std::shuffle(ids.begin(), ids.end(), rng);
    }
    std::size_t cursor = 0;
    bench_measure<Book>(
        shuffled ? "exist_shuffle" : "exist_seq", name, "ns/call", 5, 2000, 128, kOrders, 32, false,
        [&](Book& book) { return bench_build_existing(book, ids); },
        [&](Book& book) {
            for (int i = 0; i < 64; ++i) {
                const auto [id, price] = ids[cursor % ids.size()];
                ++cursor;
                bench_sink() += static_cast<std::uint64_t>(book.cancel(id));
                bench_sink() += static_cast<std::uint64_t>(book.add(id, true, price, 1));
            }
        });
}

template <typename Book>
void bench_churn(const char* name, int levels) {
    struct Window {
        std::int64_t lo{1};
        std::int64_t hi{0};
        std::uint64_t next{0};
    };
    Window window;
    auto slide = [&](Book& book, int steps) {
        for (int i = 0; i < steps; ++i) {
            const bool cancelled = book.cancel(static_cast<std::uint64_t>(window.lo));
            const bool added = book.add(window.next, false, window.hi + 1, 1);
            if (!cancelled || !added) {
                bench_failed() = true;
            }
            bench_sink() += static_cast<std::uint64_t>(cancelled) + static_cast<std::uint64_t>(added);
            ++window.lo;
            ++window.hi;
            ++window.next;
        }
    };
    const int warmup = 256;
    const std::size_t cap = static_cast<std::size_t>(levels);
    bench_measure<Book>(
        levels == 1000 ? "churn_1k" : "churn_10k", name, "ns/churn", 5, 400, 16, cap, cap, false,
        [&](Book& book) {
            window = {};
            window.lo = 1;
            window.hi = levels;
            window.next = static_cast<std::uint64_t>(levels + 1);
            for (int i = 1; i <= levels; ++i) {
                if (!book.add(static_cast<std::uint64_t>(i), false, i, 1)) {
                    return false;
                }
            }
            slide(book, warmup);
            return true;
        },
        [&](Book& book) { slide(book, 16); });
}

template <typename Book>
void bench_sweep(const char* name, const char* workload, bool across_pages) {
    constexpr int kLevels = 256;
    bench_measure<Book>(
        workload, name, "ns/sweep", 5, 500, 8, kLevels, static_cast<std::size_t>(kLevels + 8), false,
        [&](Book& book) {
            for (int i = 1; i <= kLevels; ++i) {
                const std::int64_t price = across_pages ? static_cast<std::int64_t>(i) * 256 : i;
                if (!book.add(static_cast<std::uint64_t>(i), true, price, 1)) {
                    return false;
                }
            }
            for (int i = 1; i <= kLevels; i += 2) {
                if (!book.cancel(static_cast<std::uint64_t>(i))) {
                    return false;
                }
            }
            return true;
        },
        [&](Book& book) {
            for (int i = 0; i < 8; ++i) {
                bench_sink() += book.sweep_bids();
            }
        });
}

template <typename Book>
void bench_sparse(const char* name) {
    constexpr int kLevels = 2048;
    // 65536 ticks = 256 FastLOB pages. Fits in int32. Skips price 0.
    std::vector<std::int64_t> prices(kLevels);
    for (int i = 0; i < kLevels; ++i) {
        prices[i] = static_cast<std::int64_t>(i + 1) * 65536;
    }
    std::size_t cursor = 0;
    bench_measure<Book>(
        "sparse_jump", name, "ns/insert", 5, 128, 16, kLevels, kLevels, true,
        [&](Book& book) {
            cursor = 0;
            (void)book;
            return true;
        },
        [&](Book& book) {
            for (int i = 0; i < 16; ++i) {
                const std::size_t index = cursor++;
                bench_sink() += static_cast<std::uint64_t>(
                    book.add(index + 1, true, prices[index], 1));
            }
        });
}

template <typename Book>
void bench_all(const char* name) {
    bench_existing<Book>(name, false);
    bench_existing<Book>(name, true);
    bench_churn<Book>(name, 1000);
    bench_churn<Book>(name, 10000);
    bench_sweep<Book>(name, "sweep_in_page", false);
    bench_sweep<Book>(name, "sweep_across", true);
    bench_sparse<Book>(name);
}
