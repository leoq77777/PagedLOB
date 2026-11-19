/// Apples-to-apples price-index benchmark.
/// Percentiles are of per-batch mean latency, not of single calls.
/// v1, the std::map index, and the paged directory are timed with the same harness.

#include "fastlob/lob_fast.hpp"
#include "fastlob/price_key.hpp"
#include "fastlob/storage_book.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0;

double percentile(std::vector<double> samples, double p) {
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

double stdev(const std::vector<double>& values) {
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

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count());
}

fastlob::BookMemory estimate_v1(std::size_t max_orders, std::size_t max_levels) {
    auto next_pow2 = [](std::size_t n) {
        std::size_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    };
    fastlob::BookMemory memory;
    memory.order_bytes = max_orders * sizeof(fastlob::Order);
    const std::size_t id_cap = next_pow2(std::max<std::size_t>(16, max_orders * 2));
    memory.id_bytes = id_cap * (sizeof(fastlob::OrderId) + sizeof(fastlob::Order*));
    memory.level_bytes = max_levels * sizeof(fastlob::PriceLevel);
    const std::size_t level_cap = next_pow2(std::max<std::size_t>(16, max_levels * 2));
    memory.price_bytes = 2 * level_cap * (sizeof(fastlob::Price) + sizeof(fastlob::PriceLevel*));
    memory.price_live_bytes = memory.price_bytes;
    memory.price_peak_bytes = memory.price_bytes;
    return memory;
}

template <typename Book>
fastlob::BookMemory footprint(const Book& book, std::size_t orders, std::size_t levels) {
    if constexpr (std::is_same_v<Book, fastlob::FastOrderBook>) {
        return estimate_v1(orders, levels);
    } else {
        return book.memory();
    }
}

struct Row {
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
    std::uint64_t price_bytes{0};
    std::uint64_t total_bytes{0};
};

std::vector<Row> g_rows;
bool g_failed = false;

template <typename Book, typename Setup, typename Batch>
void measure(const char* workload, const char* book_name, const char* unit, int reps, int batches,
             int ops_per_batch, std::size_t cap_orders, std::size_t cap_levels, bool memory_after_work,
             Setup&& setup, Batch&& batch) {
    std::vector<double> pooled;
    std::vector<double> rep_means;
    pooled.reserve(static_cast<std::size_t>(reps * batches));
    fastlob::BookMemory memory{};
    double seconds = 0;
    std::uint64_t ops = 0;
    for (int rep = 0; rep < reps; ++rep) {
        Book book(cap_orders, cap_levels);
        if (!setup(book)) {
            std::cerr << "setup failed " << workload << " " << book_name << "\n";
            g_failed = true;
            return;
        }
        if (rep == 0 && !memory_after_work) {
            memory = footprint(book, cap_orders, cap_levels);
        }
        for (int i = 0; i < batches; ++i) {
            const std::uint64_t t0 = now_ns();
            batch(book);
            const std::uint64_t t1 = now_ns();
            const double ns = static_cast<double>(t1 - t0) / static_cast<double>(ops_per_batch);
            pooled.push_back(ns);
            seconds += static_cast<double>(t1 - t0) * 1e-9;
            ops += static_cast<std::uint64_t>(ops_per_batch);
        }
        if (rep == 0 && memory_after_work) {
            memory = footprint(book, cap_orders, cap_levels);
        }
        double sum = 0;
        for (int i = 0; i < batches; ++i) {
            sum += pooled[pooled.size() - static_cast<std::size_t>(batches - i)];
        }
        rep_means.push_back(sum / static_cast<double>(batches));
    }
    Row row;
    row.workload = workload;
    row.book = book_name;
    row.unit = unit;
    row.batch = ops_per_batch;
    row.samples = static_cast<int>(pooled.size());
    row.mops = seconds > 0 ? (static_cast<double>(ops) / seconds) / 1e6 : 0;
    row.p50 = percentile(pooled, 0.50);
    row.p99 = percentile(pooled, 0.99);
    row.p999 = percentile(pooled, 0.999);
    row.p50_sd = stdev(rep_means);
    row.price_bytes = memory.price_bytes;
    row.total_bytes = memory.total_bytes();
    g_rows.push_back(row);
}

template <typename Book>
bool build_existing(Book& book, const std::vector<std::pair<fastlob::OrderId, fastlob::Price>>& ids) {
    for (const auto& [id, price] : ids) {
        if (!book.add(id, fastlob::Side::Buy, price, 1)) {
            return false;
        }
    }
    for (const auto& [id, price] : ids) {
        if (!book.cancel(id) || !book.add(id, fastlob::Side::Buy, price, 1)) {
            return false;
        }
    }
    return book.order_count() == ids.size();
}

template <typename Book>
void bench_existing(const char* name, bool shuffled) {
    constexpr int kOrders = 16384;
    constexpr int kPrices = 8;
    std::vector<std::pair<fastlob::OrderId, fastlob::Price>> ids;
    ids.reserve(kOrders);
    for (int i = 0; i < kOrders; ++i) {
        ids.emplace_back(static_cast<fastlob::OrderId>(i + 1), static_cast<fastlob::Price>(i % kPrices));
    }
    if (shuffled) {
        std::mt19937 rng(12345);
        std::shuffle(ids.begin(), ids.end(), rng);
    }
    std::size_t cursor = 0;
    measure<Book>(
        shuffled ?         "exist_shuffle" : "exist_seq", name, "ns/call", /*reps=*/5, /*batches=*/2000,
        /*ops_per_batch=*/128, kOrders, /*cap_levels=*/32, /*memory_after_work=*/false,
        [&](Book& book) { return build_existing(book, ids); },
        [&](Book& book) {
            for (int i = 0; i < 64; ++i) {
                const auto [id, price] = ids[cursor % ids.size()];
                ++cursor;
                g_sink += static_cast<std::uint64_t>(book.cancel(id));
                g_sink += static_cast<std::uint64_t>(book.add(id, fastlob::Side::Buy, price, 1));
            }
        });
}

template <typename Book>
void bench_churn(const char* name, int levels) {
    struct Window {
        fastlob::Price lo{0};
        fastlob::Price hi{0};
        fastlob::OrderId next{0};
    };
    Window window;
    auto slide = [&](Book& book, int steps) {
        for (int i = 0; i < steps; ++i) {
            const bool cancelled = book.cancel(static_cast<fastlob::OrderId>(window.lo + 1));
            const bool added = book.add(window.next, fastlob::Side::Sell, window.hi + 1, 1);
            if (!cancelled || !added) {
                g_failed = true;
            }
            g_sink += static_cast<std::uint64_t>(cancelled) + static_cast<std::uint64_t>(added);
            ++window.lo;
            ++window.hi;
            ++window.next;
        }
    };
    const int warmup = 256;
    // Live orders and levels stay at `levels`: each churn cancels one before adding one.
    const std::size_t cap = static_cast<std::size_t>(levels);
    measure<Book>(
        levels == 1000 ? "churn_1k" : "churn_10k", name, "ns/churn", /*reps=*/5, /*batches=*/400,
        /*ops_per_batch=*/16, cap, cap, /*memory_after_work=*/false,
        [&](Book& book) {
            window = {};
            window.hi = levels - 1;
            window.next = static_cast<fastlob::OrderId>(levels + 1);
            for (int i = 0; i < levels; ++i) {
                if (!book.add(static_cast<fastlob::OrderId>(i + 1), fastlob::Side::Sell, i, 1)) {
                    return false;
                }
            }
            slide(book, warmup);
            return book.order_count() == static_cast<std::size_t>(levels);
        },
        [&](Book& book) { slide(book, 16); });
}

template <typename Book>
void bench_sweep(const char* name, const char* workload, bool across_pages) {
    constexpr int kLevels = 256;
    std::vector<fastlob::LevelView> out(kLevels);
    measure<Book>(
        workload, name, "ns/sweep", /*reps=*/5, /*batches=*/500, /*ops_per_batch=*/8,
        /*cap_orders=*/kLevels, /*cap_levels=*/kLevels + 8, /*memory_after_work=*/false,
        [&](Book& book) {
            for (int i = 0; i < kLevels; ++i) {
                // Page 0 slot 0 is INT64_MIN, which v1's price hash uses as an empty sentinel.
                const fastlob::Price price = across_pages ? static_cast<fastlob::Price>(i) * 256 : i;
                if (!book.add(static_cast<fastlob::OrderId>(i + 1), fastlob::Side::Sell, price, 1)) {
                    return false;
                }
            }
            for (int i = 0; i < kLevels; i += 2) {
                if (!book.cancel(static_cast<fastlob::OrderId>(i + 1))) {
                    return false;
                }
            }
            const auto n = book.top_n(fastlob::Side::Sell, out);
            return n == static_cast<std::size_t>(kLevels / 2);
        },
        [&](Book& book) {
            for (int i = 0; i < 8; ++i) {
                const auto n = book.top_n(fastlob::Side::Sell, out);
                g_sink += n;
                g_sink += static_cast<std::uint64_t>(out.empty() ? 0 : out[0].price);
            }
        });
}

template <typename Book>
void bench_sparse(const char* name) {
    constexpr int kLevels = 2048;
    std::vector<fastlob::Price> prices(kLevels);
    for (int i = 0; i < kLevels; ++i) {
        // 2^20 pages apart: the 56-bit page keys diverge in the high radix digits.
        // Skip page 0 so the first tick is not INT64_MIN (v1 cannot store that sentinel).
        // Keys increase with i and the side is Buy, so each insert is a new best bid.
        // v1 links that at the head. The paged book still allocates a new radix path.
        prices[i] = fastlob::tick_from_page((static_cast<std::uint64_t>(i) + 1) << 20, 0);
    }
    std::size_t cursor = 0;
    measure<Book>(
        "sparse_jump", name, "ns/insert", /*reps=*/5, /*batches=*/128, /*ops_per_batch=*/16, kLevels,
        kLevels, /*memory_after_work=*/true,
        [&](Book& book) {
            cursor = 0;
            (void)book;
            return true;
        },
        [&](Book& book) {
            for (int i = 0; i < 16; ++i) {
                const std::size_t index = cursor++;
                g_sink += static_cast<std::uint64_t>(book.add(static_cast<fastlob::OrderId>(index + 1),
                                                              fastlob::Side::Buy, prices[index], 1));
            }
        });
}

template <typename Book>
void run_all(const char* name) {
    bench_existing<Book>(name, false);
    bench_existing<Book>(name, true);
    bench_churn<Book>(name, 1000);
    bench_churn<Book>(name, 10000);
    bench_sweep<Book>(name, "sweep_in_page", false);
    bench_sweep<Book>(name, "sweep_across", true);
    bench_sparse<Book>(name);
}

void print_table() {
    std::cout << std::fixed;
    std::cout << "BEGIN_MD\n";
    std::cout << "| workload | book | unit | batch | samples | Mops | p50 | p99 | p99.9 | p50 sd | "
                 "price index bytes | total bytes |\n";
    std::cout << "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const Row& row : g_rows) {
        std::cout << "| " << row.workload << " | " << row.book << " | " << row.unit << " | " << row.batch
                  << " | " << row.samples << " | " << std::setprecision(2) << row.mops << " | "
                  << std::setprecision(1) << row.p50 << " | " << row.p99 << " | " << row.p999 << " | "
                  << row.p50_sd << " | " << row.price_bytes << " | " << row.total_bytes << " |\n";
    }
    std::cout << "END_MD\n";
    std::cout << "dir_node_bytes " << fastlob::PagedPriceIndex<8>::dir_node_bytes() << "\n";
    std::cout << "leaf_bytes " << fastlob::PagedPriceIndex<8>::leaf_bytes() << "\n";
    std::cout << "sink " << g_sink << "\n";
}

}  // namespace

int main() {
#ifndef NDEBUG
    std::cerr << "refusing to benchmark a non-NDEBUG binary\n";
    return 2;
#endif
    std::cout << "=== price index bench ===\n";
    std::cout << "compiler: " << __VERSION__ << "\n";
    std::cout << "timer: steady_clock batch-averaged means; p50/p99/p99.9 are percentiles of those "
                 "means, pooled across reps\n";
    std::cout << "p50 sd: sample standard deviation of the per-rep mean latency\n";
    std::cout << "memory: bytes reserved by the structures after setup (v1 is fully pre-sized; "
                 "paged price bytes include the free-list reserve; map price bytes are live node "
                 "bytes). malloc rounding is not included.\n";
    {
        std::ifstream cpu("/proc/cpuinfo");
        std::string line;
        int shown = 0;
        while (std::getline(cpu, line) && shown < 1) {
            if (line.rfind("model name", 0) == 0) {
                std::cout << line << "\n";
                ++shown;
            }
        }
    }

    {
        constexpr int kBatch = 128;
        std::vector<double> samples;
        for (int i = 0; i < 2000; ++i) {
            const std::uint64_t t0 = now_ns();
            for (int j = 0; j < kBatch; ++j) {
                g_sink += static_cast<std::uint64_t>(j);
            }
            const std::uint64_t t1 = now_ns();
            samples.push_back(static_cast<double>(t1 - t0) / kBatch);
        }
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "timer_floor_ns p50 " << percentile(samples, 0.50) << " p99 " << percentile(samples, 0.99)
                  << " (empty loop, not subtracted)\n";
    }

    run_all<fastlob::FastOrderBook>("v1");
    run_all<fastlob::MapStorageBook>("stdmap");
    run_all<fastlob::PagedStorageBook>("paged");
    print_table();
    return g_failed ? 1 : 0;
}
