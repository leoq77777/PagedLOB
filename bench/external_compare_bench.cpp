#include "external/harness.hpp"

#include <iostream>

std::vector<BenchRow>& bench_rows() {
    static std::vector<BenchRow> rows;
    return rows;
}

bool& bench_failed() {
    static bool failed = false;
    return failed;
}

volatile std::uint64_t& bench_sink() {
    static volatile std::uint64_t sink = 0;
    return sink;
}

void run_fastlob();
void run_liquibook();
void run_tzadiko();
void run_br();

int main() {
#ifndef NDEBUG
    std::cerr << "refusing to benchmark a non-NDEBUG binary\n";
    return 2;
#endif
    std::cout << "=== external order book comparison ===\n";
    std::cout << "compiler: " << __VERSION__ << "\n";
    std::cout << "timer: steady_clock batch-averaged means. p50/p99/p99.9 are percentiles of those "
                 "means, pooled across 5 reps.\n";
    std::cout << "p50 sd: sample standard deviation of the per-rep mean.\n";
    std::cout << "heap_bytes: glibc mallinfo2 (uordblks + hblkhd) delta while the filled book is "
                 "alive. Includes allocator overhead. Not the same column as price_index_bench's "
                 "structural sizeof sums.\n";
    std::cout << "prices are positive and fit in int32. Price 0 is avoided (Liquibook market sentinel).\n";
    std::cout << "exist/churn/sweep/sparse stay non-crossing. match_touch and match_walk8 go through "
                 "each book's matching path (PagedLOB::match_limit; others match inside add).\n";
    std::cout << "v1 is not in this harness.\n";

    run_fastlob();
    run_liquibook();
    run_tzadiko();
    run_br();

    std::cout << std::fixed;
    std::cout << "BEGIN_MD\n";
    std::cout << "| workload | book | unit | batch | samples | Mops | p50 | p99 | p99.9 | p50 sd | heap bytes |\n";
    std::cout << "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const BenchRow& row : bench_rows()) {
        std::cout << "| " << row.workload << " | " << row.book << " | " << row.unit << " | " << row.batch
                  << " | " << row.samples << " | " << std::setprecision(2) << row.mops << " | "
                  << std::setprecision(1) << row.p50 << " | " << row.p99 << " | " << row.p999 << " | "
                  << row.p50_sd << " | " << row.heap_bytes << " |\n";
    }
    std::cout << "END_MD\n";
    std::cout << "sink " << bench_sink() << "\n";
    return bench_failed() ? 1 : 0;
}
