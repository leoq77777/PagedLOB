#include "fastlob/lob_fast.hpp"
#include "fastlob/lob_reference.hpp"
#include "fastlob/synthetic_mbo.hpp"

#include <benchmark/benchmark.h>

#include <utility>
#include <vector>

namespace {

struct Workload {
    std::vector<fastlob::BookEvent> events;
    std::vector<std::pair<fastlob::OrderId, fastlob::Price>> cancel_readd;
};

Workload make_workload() {
    fastlob::SyntheticConfig cfg;
    cfg.seed = 123;
    cfg.num_events = 20000;
    Workload w;
    w.events = fastlob::generate_synthetic_mbo(cfg);

    fastlob::OrderId id = 50'000'000;
    const auto mid = cfg.mid;
    for (int i = 0; i < 1000; ++i) {
        const fastlob::Price px = mid - 1 - (i % 5);
        w.cancel_readd.emplace_back(id, px);
        ++id;
    }
    return w;
}

const Workload& shared_workload() {
    static Workload w = make_workload();
    return w;
}

}  // namespace

static void BM_Reference_ApplyStream(benchmark::State& state) {
    const auto& w = shared_workload();
    for (auto _ : state) {
        state.PauseTiming();
        fastlob::ReferenceOrderBook book;
        state.ResumeTiming();
        for (const auto& e : w.events) {
            book.apply(e);
        }
        benchmark::DoNotOptimize(book.order_count());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(w.events.size()));
}
BENCHMARK(BM_Reference_ApplyStream);

static void BM_Fast_ApplyStream(benchmark::State& state) {
    const auto& w = shared_workload();
    for (auto _ : state) {
        state.PauseTiming();
        // Capacity sized for this workload; construction excluded from timing.
        fastlob::FastOrderBook book(1 << 16, 1 << 12);
        state.ResumeTiming();
        for (const auto& e : w.events) {
            book.apply(e);
        }
        benchmark::DoNotOptimize(book.order_count());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(w.events.size()));
}
BENCHMARK(BM_Fast_ApplyStream);

static void BM_Fast_CancelReadd(benchmark::State& state) {
    const auto& w = shared_workload();
    fastlob::FastOrderBook book(1 << 16, 1 << 12);
    for (const auto& e : w.events) {
        book.apply(e);
    }
    for (const auto& [id, px] : w.cancel_readd) {
        book.add(id, fastlob::Side::Buy, px, 10);
    }

    std::size_t i = 0;
    for (auto _ : state) {
        const auto& [id, px] = w.cancel_readd[i % w.cancel_readd.size()];
        book.cancel(id);
        book.add(id, fastlob::Side::Buy, px, 10);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Fast_CancelReadd);
