/// Page-width parameter sweep for PagedLOB.
/// Instantiates 32/64/128/256/512/1024-tick leaves and runs the same workloads.

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
#include <string>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0;
bool g_failed = false;

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

struct Row {
    std::string workload;
    std::string book;
    std::string unit;
    int ticks{0};
    double p50{0};
    double p99{0};
    double p50_sd{0};
    std::uint64_t price_bytes{0};
};

std::vector<Row> g_rows;

template <typename Book, typename Setup, typename Batch>
void measure(const char* workload, const char* book_name, int ticks, const char* unit, int reps,
             int batches, int ops_per_batch, std::size_t cap_orders, std::size_t cap_levels,
             bool memory_after_work, Setup&& setup, Batch&& batch) {
    std::vector<double> pooled;
    std::vector<double> rep_means;
    pooled.reserve(static_cast<std::size_t>(reps * batches));
    fastlob::BookMemory memory{};
    for (int rep = 0; rep < reps; ++rep) {
        Book book(cap_orders, cap_levels);
        if (!setup(book)) {
            std::cerr << "setup failed " << workload << " " << book_name << "\n";
            g_failed = true;
            return;
        }
        if (rep == 0 && !memory_after_work) {
            memory = book.memory();
        }
        for (int i = 0; i < batches; ++i) {
            const std::uint64_t t0 = now_ns();
            batch(book);
            const std::uint64_t t1 = now_ns();
            pooled.push_back(static_cast<double>(t1 - t0) / static_cast<double>(ops_per_batch));
        }
        if (rep == 0 && memory_after_work) {
            memory = book.memory();
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
    row.ticks = ticks;
    row.p50 = percentile(pooled, 0.50);
    row.p99 = percentile(pooled, 0.99);
    row.p50_sd = stdev(rep_means);
    row.price_bytes = memory.price_bytes;
    g_rows.push_back(row);
}

template <int PageBits>
const char* name_of() {
    switch (PageBits) {
        case 5:
            return "p32";
        case 6:
            return "p64";
        case 7:
            return "p128";
        case 8:
            return "p256";
        case 9:
            return "p512";
        case 10:
            return "p1024";
        default:
            return "p?";
    }
}

template <int PageBits>
void bench_existing() {
    using Book = fastlob::PagedStorageBookT<PageBits>;
    constexpr int kOrders = 16384;
    constexpr int kPrices = 8;
    std::vector<std::pair<fastlob::OrderId, fastlob::Price>> ids;
    ids.reserve(kOrders);
    for (int i = 0; i < kOrders; ++i) {
        ids.emplace_back(static_cast<fastlob::OrderId>(i + 1), static_cast<fastlob::Price>(i % kPrices));
    }
    std::size_t cursor = 0;
    measure<Book>(
        "exist_seq", name_of<PageBits>(), 1 << PageBits, "ns/call", 5, 2000, 128, kOrders, 32, false,
        [&](Book& book) {
            for (const auto& [id, price] : ids) {
                if (!book.add(id, fastlob::Side::Buy, price, 1)) {
                    return false;
                }
            }
            return book.order_count() == ids.size();
        },
        [&](Book& book) {
            for (int i = 0; i < 64; ++i) {
                const auto [id, price] = ids[cursor % ids.size()];
                ++cursor;
                g_sink += static_cast<std::uint64_t>(book.cancel(id));
                g_sink += static_cast<std::uint64_t>(book.add(id, fastlob::Side::Buy, price, 1));
            }
        });
}

template <int PageBits>
void bench_churn(int levels, const char* workload) {
    using Book = fastlob::PagedStorageBookT<PageBits>;
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
    const std::size_t cap = static_cast<std::size_t>(levels);
    measure<Book>(
        workload, name_of<PageBits>(), 1 << PageBits, "ns/churn", 5, 400, 16, cap, cap, false,
        [&](Book& book) {
            window = {};
            window.hi = levels - 1;
            window.next = static_cast<fastlob::OrderId>(levels + 1);
            for (int i = 0; i < levels; ++i) {
                if (!book.add(static_cast<fastlob::OrderId>(i + 1), fastlob::Side::Sell, i, 1)) {
                    return false;
                }
            }
            slide(book, 256);
            return book.order_count() == cap;
        },
        [&](Book& book) { slide(book, 16); });
}

template <int PageBits>
void bench_walk(const char* workload, bool across_pages) {
    using Book = fastlob::PagedStorageBookT<PageBits>;
    constexpr int kLevels = 256;
    constexpr int kTicks = 1 << PageBits;
    std::vector<fastlob::LevelView> out(kLevels);
    measure<Book>(
        workload, name_of<PageBits>(), kTicks, "ns/sweep", 5, 500, 8, kLevels, kLevels + 8, false,
        [&](Book& book) {
            for (int i = 0; i < kLevels; ++i) {
                const fastlob::Price price =
                    across_pages ? static_cast<fastlob::Price>(i) * kTicks : i;
                if (!book.add(static_cast<fastlob::OrderId>(i + 1), fastlob::Side::Sell, price, 1)) {
                    return false;
                }
            }
            for (int i = 0; i < kLevels; i += 2) {
                if (!book.cancel(static_cast<fastlob::OrderId>(i + 1))) {
                    return false;
                }
            }
            return book.top_n(fastlob::Side::Sell, out) == static_cast<std::size_t>(kLevels / 2);
        },
        [&](Book& book) {
            for (int i = 0; i < 8; ++i) {
                const auto n = book.top_n(fastlob::Side::Sell, out);
                g_sink += n;
                g_sink += static_cast<std::uint64_t>(out.empty() ? 0 : out[0].price);
            }
        });
}

template <int PageBits>
void bench_sparse() {
    using Book = fastlob::PagedStorageBookT<PageBits>;
    constexpr int kLevels = 2048;
    std::vector<fastlob::Price> prices(kLevels);
    for (int i = 0; i < kLevels; ++i) {
        prices[i] = fastlob::tick_from_page<PageBits>((static_cast<std::uint64_t>(i) + 1) << 20, 0);
    }
    std::size_t cursor = 0;
    measure<Book>(
        "sparse_jump", name_of<PageBits>(), 1 << PageBits, "ns/insert", 5, 128, 16, kLevels, kLevels, true,
        [&](Book& book) {
            cursor = 0;
            (void)book;
            return true;
        },
        [&](Book& book) {
            for (int i = 0; i < 16; ++i) {
                const std::size_t index = cursor++;
                g_sink += static_cast<std::uint64_t>(
                    book.add(static_cast<fastlob::OrderId>(index + 1), fastlob::Side::Buy, prices[index], 1));
            }
        });
}

template <int PageBits>
void run_variant() {
    using Index = fastlob::PagedPriceIndex<PageBits>;
    std::cout << name_of<PageBits>() << " ticks=" << Index::kTicks << " digits=" << Index::kDigits
              << " last_dir=" << Index::kLastDirDepth << " dir_bytes=" << Index::dir_node_bytes()
              << " leaf_bytes=" << Index::leaf_bytes() << "\n";
    bench_existing<PageBits>();
    bench_churn<PageBits>(1000, "churn_1k");
    bench_churn<PageBits>(10000, "churn_10k");
    bench_walk<PageBits>("walk_dense", false);
    bench_walk<PageBits>("walk_1_per_page", true);
    bench_sparse<PageBits>();
}

void print_table() {
    std::cout << std::fixed;
    std::cout << "BEGIN_MD\n";
    std::cout << "| workload | book | ticks | unit | p50 | p99 | p50 sd | price index bytes |\n";
    std::cout << "|---|---|---:|---|---:|---:|---:|---:|\n";
    for (const Row& row : g_rows) {
        std::cout << "| " << row.workload << " | " << row.book << " | " << row.ticks << " | " << row.unit
                  << " | " << std::setprecision(1) << row.p50 << " | " << row.p99 << " | " << row.p50_sd
                  << " | " << row.price_bytes << " |\n";
    }
    std::cout << "END_MD\n";
}

}  // namespace

int main() {
#ifndef NDEBUG
    std::cerr << "refusing to benchmark a non-NDEBUG binary\n";
    return 2;
#endif
    std::cout << "=== page-width parameter sweep ===\n";
    std::cout << "compiler: " << __VERSION__ << "\n";
    {
        std::ifstream cpu("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpu, line)) {
            if (line.rfind("model name", 0) == 0) {
                std::cout << line << "\n";
                break;
            }
        }
    }

    run_variant<5>();
    run_variant<6>();
    run_variant<7>();
    run_variant<8>();
    run_variant<9>();
    run_variant<10>();
    print_table();
    std::cout << "sink " << g_sink << "\n";
    return g_failed ? 1 : 0;
}
