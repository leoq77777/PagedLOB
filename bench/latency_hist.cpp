/// Per-op latency histogram for Fast LOB cancel+re-add.
/// Uses batched timing so results are not dominated by timer quantum (~40ns on
/// some Apple platforms when measuring a single op with steady_clock).

#include "fastlob/lob_fast.hpp"
#include "fastlob/lob_reference.hpp"
#include "fastlob/synthetic_mbo.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace {

struct Workload {
    std::vector<fastlob::BookEvent> events;
    std::vector<std::pair<fastlob::OrderId, fastlob::Price>> ids;
};

Workload make_workload(std::size_t stream_events, std::size_t warm_ids) {
    fastlob::SyntheticConfig cfg;
    cfg.seed = 123;
    cfg.num_events = stream_events;
    Workload w;
    w.events = fastlob::generate_synthetic_mbo(cfg);
    fastlob::OrderId id = 50'000'000;
    for (std::size_t i = 0; i < warm_ids; ++i) {
        const fastlob::Price px = cfg.mid - 1 - static_cast<fastlob::Price>(i % 5);
        w.ids.emplace_back(id++, px);
    }
    return w;
}

#if defined(__APPLE__)
class NanoClock {
public:
    NanoClock() {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        numer_ = info.numer;
        denom_ = info.denom;
    }

    [[nodiscard]] std::uint64_t now_ns() const {
        return mach_absolute_time() * numer_ / denom_;
    }

private:
    std::uint32_t numer_{1};
    std::uint32_t denom_{1};
};
#else
class NanoClock {
public:
    [[nodiscard]] std::uint64_t now_ns() const {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    }
};
#endif

double percentile(std::vector<double>& samples, double p) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const double idx = p * static_cast<double>(samples.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, samples.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return samples[lo] * (1.0 - frac) + samples[hi] * frac;
}

struct Stats {
    double p50{0};
    double p99{0};
    double p999{0};
    double mean{0};
    double max{0};
    std::size_t n{0};
    std::size_t batch{0};
    std::size_t ops{0};
};

Stats summarize(std::vector<double> samples, std::size_t batch, std::size_t ops) {
    Stats s;
    s.n = samples.size();
    s.batch = batch;
    s.ops = ops;
    if (samples.empty()) {
        return s;
    }
    s.mean = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
    s.max = *std::max_element(samples.begin(), samples.end());
    s.p50 = percentile(samples, 0.50);
    s.p99 = percentile(samples, 0.99);
    s.p999 = percentile(samples, 0.999);
    return s;
}

void print_stats(const char* name, const Stats& s) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << name << "\n"
              << "  samples: " << s.n << " (batch=" << s.batch << ", total_ops=" << s.ops << ")\n"
              << "  p50_ns:  " << s.p50 << "\n"
              << "  p99_ns:  " << s.p99 << "\n"
              << "  p999_ns: " << s.p999 << "\n"
              << "  mean_ns: " << s.mean << "\n"
              << "  max_ns:  " << s.max << "\n";
}

template <typename Book>
Stats measure_cancel_readd(Book& book,
                           const std::vector<std::pair<fastlob::OrderId, fastlob::Price>>& ids,
                           std::size_t total_ops,
                           std::size_t batch,
                           std::size_t warmup_ops) {
    for (const auto& [id, px] : ids) {
        book.add(id, fastlob::Side::Buy, px, 10);
    }

    NanoClock clock;
    std::size_t cursor = 0;
    auto step = [&]() {
        const auto& [id, px] = ids[cursor % ids.size()];
        book.cancel(id);
        book.add(id, fastlob::Side::Buy, px, 10);
        ++cursor;
    };

    for (std::size_t i = 0; i < warmup_ops; ++i) {
        step();
    }

    const std::size_t batches = total_ops / batch;
    std::vector<double> samples;
    samples.reserve(batches);
    for (std::size_t b = 0; b < batches; ++b) {
        const std::uint64_t t0 = clock.now_ns();
        for (std::size_t i = 0; i < batch; ++i) {
            step();
        }
        const std::uint64_t t1 = clock.now_ns();
        samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(batch));
    }
    return summarize(std::move(samples), batch, batches * batch);
}

Stats measure_stream_ns_per_event(bool fast, const Workload& w, std::size_t rounds) {
    NanoClock clock;
    std::vector<double> samples;
    samples.reserve(rounds);
    for (std::size_t r = 0; r < rounds; ++r) {
        if (fast) {
            fastlob::FastOrderBook book(1 << 16, 1 << 12);
            const std::uint64_t t0 = clock.now_ns();
            for (const auto& e : w.events) {
                book.apply(e);
            }
            const std::uint64_t t1 = clock.now_ns();
            samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(w.events.size()));
            (void)book.order_count();
        } else {
            fastlob::ReferenceOrderBook book;
            const std::uint64_t t0 = clock.now_ns();
            for (const auto& e : w.events) {
                book.apply(e);
            }
            const std::uint64_t t1 = clock.now_ns();
            samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(w.events.size()));
            (void)book.order_count();
        }
    }
    return summarize(std::move(samples), w.events.size(), rounds * w.events.size());
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t burst = 1'000'000;
    std::size_t batch = 64;
    std::size_t warm_ids = 1000;
    std::size_t stream_events = 20'000;
    std::size_t stream_rounds = 50;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* flag) -> std::size_t {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                std::exit(2);
            }
            return static_cast<std::size_t>(std::stoull(argv[++i]));
        };
        if (a == "--burst") {
            burst = need("--burst");
        } else if (a == "--batch") {
            batch = need("--batch");
        } else if (a == "--warm-ids") {
            warm_ids = need("--warm-ids");
        } else if (a == "--stream-events") {
            stream_events = need("--stream-events");
        } else if (a == "--stream-rounds") {
            stream_rounds = need("--stream-rounds");
        } else if (a == "--help") {
            std::cout << "Usage: latency_hist [--burst N] [--batch N] [--warm-ids N] "
                         "[--stream-events N] [--stream-rounds N]\n";
            return 0;
        }
    }

    if (batch == 0 || burst < batch) {
        std::cerr << "burst must be >= batch > 0\n";
        return 2;
    }

#if defined(__APPLE__)
    const char* timer = "mach_absolute_time";
#else
    const char* timer = "steady_clock";
#endif

    std::cout << "=== FastLOB latency histogram ===\n";
    std::cout << "timer: " << timer << "\n";
    std::cout << "workload: synthetic MBO stream_events=" << stream_events
              << " seed=123; cancel+re-add burst=" << burst << " batch=" << batch
              << " warm_ids=" << warm_ids << "\n";

    const Workload w = make_workload(stream_events, warm_ids);

    {
        fastlob::FastOrderBook book(1 << 18, 1 << 14);
        for (const auto& e : w.events) {
            book.apply(e);
        }
        auto s = measure_cancel_readd(book, w.ids, burst, batch, /*warmup_ops=*/10000);
        print_stats("Fast cancel+re-add (hot cache, batched)", s);
    }

    {
        fastlob::ReferenceOrderBook book;
        for (const auto& e : w.events) {
            book.apply(e);
        }
        const std::size_t ref_burst = std::min<std::size_t>(burst, 200'000);
        auto s = measure_cancel_readd(book, w.ids, ref_burst, batch, /*warmup_ops=*/2000);
        print_stats("Reference cancel+re-add (batched)", s);
    }

    print_stats("Fast apply-stream ns/event", measure_stream_ns_per_event(true, w, stream_rounds));
    print_stats("Reference apply-stream ns/event",
                measure_stream_ns_per_event(false, w, stream_rounds));

    return 0;
}
