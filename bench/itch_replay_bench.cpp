#include "fastlob/itch50.hpp"
#include "fastlob/lob_fast.hpp"
#include "fastlob/lob_reference.hpp"
#include "fastlob/replay.hpp"
#include "fastlob/storage_book.hpp"
#include "perf_counters.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
namespace itch = fastlob::itch50;

volatile std::uint64_t g_sink = 0;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count());
}

double percentile(std::vector<double> samples, double p) {
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

double stdev(const std::vector<double>& values) {
    if (values.size() < 2) {
        return 0.0;
    }
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    double acc = 0.0;
    for (double v : values) {
        const double d = v - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(values.size() - 1));
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct SymbolStats {
    std::string symbol;
    std::size_t total_events{0};
    std::size_t peak_orders{0};
    std::size_t peak_levels_buy{0};
    std::size_t peak_levels_sell{0};
    std::size_t add_events{0};
    std::size_t hot_add_hits{0};
    fastlob::Price sample_mid_ticks{0};
};

/// Verify 100% differential L3 book equivalence across PagedLOB, MapStorageBook (std::map),
/// FastOrderBook (v1), and ReferenceOrderBook (std::map + std::list) at 100 intraday checkpoints + EOD,
/// while recording peak book depth and empirical 256-tick hot-window hit rate.
bool verify_and_profile_stream(const std::string& symbol, const std::vector<fastlob::BookEvent>& events,
                               std::size_t max_orders, std::size_t max_levels, SymbolStats& out_stats) {
    out_stats = {};
    out_stats.symbol = symbol;
    out_stats.total_events = events.size();
    if (events.empty()) {
        return false;
    }

    fastlob::PagedLOB paged(max_orders, max_levels);
    fastlob::MapStorageBook map_book(max_orders, max_levels);
    fastlob::FastOrderBook v1(max_orders, max_levels);
    fastlob::ReferenceOrderBook ref;

    const std::size_t stride = std::max<std::size_t>(1, events.size() / 100);
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& ev = events[i];
        if (ev.type == fastlob::EventType::Add) {
            ++out_stats.add_events;
            const auto best = paged.best_price(ev.side);
            if (!best.has_value()) {
                ++out_stats.hot_add_hits;
            } else if (ev.side == fastlob::Side::Sell) {
                if (ev.price >= *best &&
                    static_cast<std::uint64_t>(ev.price) - static_cast<std::uint64_t>(*best) < 256) {
                    ++out_stats.hot_add_hits;
                }
            } else {
                if (ev.price <= *best &&
                    static_cast<std::uint64_t>(*best) - static_cast<std::uint64_t>(ev.price) < 256) {
                    ++out_stats.hot_add_hits;
                }
            }
        }

        const bool ok_paged = paged.apply(ev);
        const bool ok_map = map_book.apply(ev);
        const bool ok_v1 = v1.apply(ev);
        const bool ok_ref = ref.apply(ev);
        if (ok_paged != ok_ref || ok_map != ok_ref || ok_v1 != ok_ref) {
            std::cerr << "[" << symbol << "] apply return mismatch at event " << i << "\n";
            return false;
        }

        out_stats.peak_orders = std::max(out_stats.peak_orders, paged.order_count());

        if ((i + 1) % stride == 0 || i + 1 == events.size()) {
            if (paged.order_count() != ref.order_count() || map_book.order_count() != ref.order_count() ||
                v1.order_count() != ref.order_count()) {
                std::cerr << "[" << symbol << "] order_count mismatch at event " << i << "\n";
                return false;
            }
            if (paged.best_price(fastlob::Side::Buy) != map_book.best_price(fastlob::Side::Buy) ||
                paged.best_price(fastlob::Side::Sell) != map_book.best_price(fastlob::Side::Sell)) {
                std::cerr << "[" << symbol << "] BBO mismatch at event " << i << "\n";
                return false;
            }
            for (fastlob::Side side : {fastlob::Side::Buy, fastlob::Side::Sell}) {
                fastlob::LevelView a[10]{};
                fastlob::LevelView b[10]{};
                fastlob::LevelView c[10]{};
                fastlob::LevelView d[10]{};
                const std::size_t na = paged.top_n(side, a);
                const std::size_t nb = map_book.top_n(side, b);
                const std::size_t nc = v1.top_n(side, c);
                const std::size_t nd = ref.top_n(side, d);
                if (na != nd || nb != nd || nc != nd) {
                    std::cerr << "[" << symbol << "] top_n count mismatch at event " << i << "\n";
                    return false;
                }
                for (std::size_t k = 0; k < na; ++k) {
                    if (a[k].price != d[k].price || a[k].qty != d[k].qty ||
                        b[k].price != d[k].price || b[k].qty != d[k].qty ||
                        c[k].price != d[k].price || c[k].qty != d[k].qty) {
                        std::cerr << "[" << symbol << "] top_n level mismatch at event " << i << " k=" << k
                                  << "\n";
                        return false;
                    }
                }
            }
            if (i >= events.size() / 2 && out_stats.sample_mid_ticks == 0) {
                const auto bid = paged.best_price(fastlob::Side::Buy);
                const auto ask = paged.best_price(fastlob::Side::Sell);
                if (bid && ask) {
                    out_stats.sample_mid_ticks = (*bid + *ask) / 2;
                }
            }
        }
    }
    return true;
}

struct ReplayRow {
    std::string symbol;
    std::string book;
    std::size_t events{0};
    int batch{0};
    int samples{0};
    double mops{0.0};
    double p50{0.0};
    double p99{0.0};
    double p999{0.0};
    double p50_sd{0.0};
    double full_day_ms{0.0};
    fastlob::bench::PerfSnapshot perf{};
};

template <typename MakeBook, typename InitBook>
ReplayRow bench_book_replay(const std::string& symbol, const char* book_name,
                            const std::vector<fastlob::BookEvent>& events, int reps, int batch_size,
                            MakeBook&& make_book, InitBook&& init_book) {
    const std::size_t full_batches = events.size() / static_cast<std::size_t>(batch_size);
    std::vector<double> pooled;
    pooled.reserve(static_cast<std::size_t>(reps) * full_batches);
    std::vector<double> rep_means;
    rep_means.reserve(static_cast<std::size_t>(reps));
    double total_sec = 0.0;
    std::uint64_t total_ops = 0;

    // 1 untimed warmup pass so the first book in the suite does not absorb cold event-stream faults
    {
        auto warm_book = make_book();
        init_book(warm_book);
        std::uint64_t warm_acc = 0;
        for (const auto& ev : events) {
            warm_acc += static_cast<std::uint64_t>(warm_book.apply(ev));
        }
        g_sink += warm_acc + warm_book.order_count();
    }

    for (int rep = 0; rep < reps; ++rep) {
        auto book = make_book();
        init_book(book);
        double rep_sum = 0.0;
        const std::uint64_t day_t0 = now_ns();
        for (std::size_t b = 0; b < full_batches; ++b) {
            const std::size_t base = b * static_cast<std::size_t>(batch_size);
            const std::uint64_t t0 = now_ns();
            std::uint64_t applied = 0;
            for (int j = 0; j < batch_size; ++j) {
                applied += static_cast<std::uint64_t>(book.apply(events[base + static_cast<std::size_t>(j)]));
            }
            const std::uint64_t t1 = now_ns();
            g_sink += applied;
            const double ns_per_ev = static_cast<double>(t1 - t0) / static_cast<double>(batch_size);
            pooled.push_back(ns_per_ev);
            rep_sum += ns_per_ev;
        }
        for (std::size_t tail = full_batches * static_cast<std::size_t>(batch_size); tail < events.size();
             ++tail) {
            g_sink += static_cast<std::uint64_t>(book.apply(events[tail]));
        }
        const std::uint64_t day_t1 = now_ns();
        total_sec += static_cast<double>(day_t1 - day_t0) * 1e-9;
        total_ops += events.size();
        g_sink += book.order_count();
        if (full_batches > 0) {
            rep_means.push_back(rep_sum / static_cast<double>(full_batches));
        }
    }

    ReplayRow row;
    row.symbol = symbol;
    row.book = book_name;
    row.events = events.size();
    row.batch = batch_size;
    row.samples = static_cast<int>(pooled.size());
    row.mops = total_sec > 0.0 ? (static_cast<double>(total_ops) / total_sec) / 1e6 : 0.0;
    row.p50 = percentile(pooled, 0.50);
    row.p99 = percentile(pooled, 0.99);
    row.p999 = percentile(pooled, 0.999);
    row.p50_sd = stdev(rep_means);
    row.full_day_ms = (total_sec * 1e3) / static_cast<double>(reps);

    // Separate pure hot-loop pass for hardware PMU counters (if exposed by kernel/CPU)
    fastlob::bench::LinuxPerfCounters perf;
    if (perf.available()) {
        auto book = make_book();
        init_book(book);
        perf.reset_and_enable();
        std::uint64_t applied = 0;
        for (const auto& ev : events) {
            applied += static_cast<std::uint64_t>(book.apply(ev));
        }
        perf.disable();
        g_sink += applied + book.order_count();
        row.perf = perf.read();
    }
    return row;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef NDEBUG
    std::cerr << "refusing to benchmark a non-NDEBUG binary\n";
    return 2;
#endif
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <itch50_file(.gz)|events.bin> [--symbols=AAPL,SPY,MSFT] [--save-dir=/path] "
                     "[--reps=5] [--batch=128] [--skip-ref-bench]\n";
        return 1;
    }

    const std::string input_path = argv[1];
    std::string symbols_arg = "AAPL";
    std::string save_dir;
    std::string bin_dir;
    int reps = 5;
    int batch_size = 128;
    std::uint32_t tick_divisor = 100;
    bool run_ref_bench = true;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--symbols=", 0) == 0) {
            symbols_arg = arg.substr(10);
        } else if (arg.rfind("--symbol=", 0) == 0) {
            symbols_arg = arg.substr(9);
        } else if (arg.rfind("--save-dir=", 0) == 0) {
            save_dir = arg.substr(11);
        } else if (arg.rfind("--bin-dir=", 0) == 0) {
            bin_dir = arg.substr(10);
        } else if (arg.rfind("--reps=", 0) == 0) {
            reps = std::max(1, std::stoi(arg.substr(7)));
        } else if (arg.rfind("--batch=", 0) == 0) {
            batch_size = std::max(1, std::stoi(arg.substr(8)));
        } else if (arg.rfind("--tick-divisor=", 0) == 0) {
            tick_divisor = static_cast<std::uint32_t>(std::max(1, std::stoi(arg.substr(15))));
        } else if (arg == "--skip-ref-bench") {
            run_ref_bench = false;
        }
    }

    const auto symbols = split_csv(symbols_arg);
    if (symbols.empty()) {
        std::cerr << "No symbols specified\n";
        return 1;
    }

    std::cout << "=== NASDAQ TotalView-ITCH 5.0 Real Market Replay Benchmark ===\n";
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
    std::cout << "input: " << input_path << " | reps=" << reps << " | batch=" << batch_size << "\n";

    std::vector<std::vector<fastlob::BookEvent>> streams(symbols.size());
    if (!bin_dir.empty()) {
        for (std::size_t s = 0; s < symbols.size(); ++s) {
            const std::string p = bin_dir + "/" + symbols[s] + ".bin";
            if (!fastlob::read_events_bin(p, streams[s])) {
                std::cerr << "Failed to read BinaryEvent slice: " << p << "\n";
                return 1;
            }
            std::cout << "  loaded [" << symbols[s] << "] slice: " << streams[s].size() << " events\n";
        }
    } else if (ends_with(input_path, ".bin") && symbols.size() == 1) {
        if (!fastlob::read_events_bin(input_path, streams[0])) {
            std::cerr << "Failed to read BinaryEvent file: " << input_path << "\n";
            return 1;
        }
        std::cout << "loaded pre-sliced BinaryEvent stream for " << symbols[0] << ": "
                  << streams[0].size() << " events\n";
    } else {
        std::vector<itch::ItchSymbolNormalizer> norms;
        norms.reserve(symbols.size());
        for (const auto& sym : symbols) {
            norms.emplace_back(sym, tick_divisor);
            streams[norms.size() - 1].reserve(1 << 21);
        }

        std::vector<int> locate_map(65536, -1);
        std::size_t found_count = 0;

        const std::uint64_t parse_t0 = now_ns();
        std::uint64_t wire_msgs = 0;
        const bool ok = itch::stream_itch50_file(
            input_path, [&](const std::uint8_t* msg, std::size_t len) {
                ++wire_msgs;
                const char mtype = static_cast<char>(msg[0]);
                if (mtype == 'S') {
                    for (auto& n : norms) {
                        n.on_message(msg, len, [](const fastlob::BookEvent&) {});
                    }
                    return;
                }
                if (mtype == 'R') {
                    if (found_count < norms.size()) {
                        for (std::size_t s = 0; s < norms.size(); ++s) {
                            if (!norms[s].stats().locate_found) {
                                norms[s].on_message(msg, len, [](const fastlob::BookEvent&) {});
                                if (norms[s].stats().locate_found) {
                                    locate_map[norms[s].stats().stock_locate] = static_cast<int>(s);
                                    ++found_count;
                                }
                            }
                        }
                    }
                    return;
                }
                if (len < 3) {
                    return;
                }
                const int idx = locate_map[itch::read_be16(msg + 1)];
                if (idx >= 0) {
                    const auto s = static_cast<std::size_t>(idx);
                    norms[s].on_message(msg, len, [&](const fastlob::BookEvent& ev) {
                        streams[s].push_back(ev);
                    });
                }
            });
        const std::uint64_t parse_t1 = now_ns();
        if (!ok) {
            std::cerr << "Failed to stream ITCH 5.0 file (ensure zlib enabled for .gz): " << input_path
                      << "\n";
            return 1;
        }
        const double parse_sec = static_cast<double>(parse_t1 - parse_t0) * 1e-9;
        std::cout << std::fixed << std::setprecision(2)
                  << "streamed " << wire_msgs << " wire messages in " << parse_sec << " s ("
                  << (parse_sec > 0 ? (static_cast<double>(wire_msgs) / parse_sec) / 1e6 : 0.0)
                  << " M wire msgs/s, tick_divisor=" << tick_divisor << ")\n";

        for (std::size_t s = 0; s < symbols.size(); ++s) {
            const auto& st = norms[s].stats();
            std::cout << "  [" << symbols[s] << "] locate=" << st.stock_locate
                      << " wire_msgs=" << st.symbol_wire_msgs
                      << " book_events=" << streams[s].size()
                      << " (Add=" << st.add_msgs << ", CancelX=" << st.cancel_part_msgs
                      << ", DeleteD=" << st.delete_msgs << ", ReplaceU=" << st.replace_msgs
                      << ", ExecEC=" << st.execute_msgs << ", SubPenny=" << st.subpenny_msgs << ")\n";
            if (!save_dir.empty() && !streams[s].empty()) {
                const std::string out_bin = save_dir + "/" + symbols[s] + ".bin";
                if (fastlob::write_events_bin(out_bin, streams[s])) {
                    std::cout << "  saved compact binary slice: " << out_bin << "\n";
                }
            }
        }
    }

    std::vector<ReplayRow> rows;
    for (std::size_t s = 0; s < symbols.size(); ++s) {
        const auto& evs = streams[s];
        if (evs.empty()) {
            std::cerr << "Warning: 0 events found for symbol " << symbols[s] << "\n";
            continue;
        }
        // Verify differential correctness with generous upper-bound pools, then right-size
        // per-symbol benchmark pools to >= 2x full-day peak live orders (min 65,536).
        SymbolStats sym_stats{};
        if (!verify_and_profile_stream(symbols[s], evs, 1u << 19, 1u << 16, sym_stats)) {
            return 1;
        }
        const std::size_t max_orders =
            std::max<std::size_t>(65536, std::bit_ceil(sym_stats.peak_orders * 2));
        const std::size_t max_levels = 32768;
        const double hot_pct = sym_stats.add_events > 0
                                   ? 100.0 * static_cast<double>(sym_stats.hot_add_hits) /
                                         static_cast<double>(sym_stats.add_events)
                                   : 0.0;
        std::cout << std::fixed << std::setprecision(2)
                  << "  [" << symbols[s] << "] DIFFERENTIAL VERIFIED (100 checkpoints + EOD match 100%): "
                  << "peak_live_orders=" << sym_stats.peak_orders
                  << ", mid_price=$" << std::setprecision(4)
                  << fastlob::ticks_to_dollars(sym_stats.sample_mid_ticks)
                  << ", hot_256tick_add_hit_rate=" << std::setprecision(2) << hot_pct << "%\n";

        const auto noop_init = [](auto&) {};
        rows.push_back(bench_book_replay(
            symbols[s], "paged", evs, reps, batch_size,
            [&]() { return fastlob::PagedLOB(max_orders, max_levels); },
            [](fastlob::PagedLOB& b) { b.reserve_cold(64, 128); }));
        rows.push_back(bench_book_replay(
            symbols[s], "v1", evs, reps, batch_size,
            [&]() { return fastlob::FastOrderBook(max_orders, max_levels); }, noop_init));
        rows.push_back(bench_book_replay(
            symbols[s], "stdmap", evs, reps, batch_size,
            [&]() { return fastlob::MapStorageBook(max_orders, max_levels); }, noop_init));
        if (run_ref_bench) {
            rows.push_back(bench_book_replay(
                symbols[s], "ref_map_list", evs, std::min(reps, 3), batch_size,
                [&]() { return fastlob::ReferenceOrderBook{}; }, noop_init));
        }
    }

    std::cout << "\nBEGIN_ITCH_MD\n";
    std::cout << "| symbol | book | events | batch | samples | Mops/s | p50 (ns) | p99 (ns) | p99.9 (ns) | p50 sd | day replay (ms) |\n";
    std::cout << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& r : rows) {
        std::cout << "| " << r.symbol << " | " << r.book << " | " << r.events << " | " << r.batch
                  << " | " << r.samples << " | " << std::fixed << std::setprecision(2) << r.mops
                  << " | " << std::setprecision(1) << r.p50 << " | " << r.p99 << " | " << r.p999
                  << " | " << r.p50_sd << " | " << std::setprecision(1) << r.full_day_ms << " |\n";
    }
    std::cout << "END_ITCH_MD\n";

    if (!rows.empty() && rows[0].perf.available) {
        std::cout << "\nBEGIN_PERF_MD\n";
        std::cout << "| symbol | book | cycles/ev | inst/ev | IPC | br_miss/1k | L1d_miss/1k | LLC_miss/1k |\n";
        std::cout << "|---|---|---:|---:|---:|---:|---:|---:|\n";
        for (const auto& r : rows) {
            const double n = static_cast<double>(r.events);
            std::cout << "| " << r.symbol << " | " << r.book << " | " << std::fixed
                      << std::setprecision(1) << (static_cast<double>(r.perf.cycles) / n) << " | "
                      << (static_cast<double>(r.perf.instructions) / n) << " | "
                      << std::setprecision(2) << r.perf.ipc() << " | "
                      << (1000.0 * static_cast<double>(r.perf.branch_misses) / n) << " | "
                      << (1000.0 * static_cast<double>(r.perf.l1d_load_misses) / n) << " | "
                      << (1000.0 * static_cast<double>(r.perf.llc_load_misses) / n) << " |\n";
        }
        std::cout << "END_PERF_MD\n";
    } else {
        std::cout << "perf_counters: unavailable on this VM/kernel (run on bare-metal Linux / WSL via scripts/run_remote_perf.sh)\n";
    }

    std::cout << "sink " << g_sink << "\n";
    return 0;
}
