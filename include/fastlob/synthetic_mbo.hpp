#pragma once

#include "fastlob/types.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace fastlob {

struct SyntheticConfig {
    std::uint64_t seed{42};
    std::size_t num_events{100000};
    Price mid{dollars_to_ticks(100.0)};
    Price tick{1};
    int levels_each_side{10};
    Qty min_qty{10};
    Qty max_qty{500};
    double cancel_prob{0.35};
    double modify_prob{0.10};
    double execute_prob{0.15};
    // remainder => add
};

/// Generate a self-consistent synthetic MBO stream (add/cancel/modify/execute).
inline std::vector<BookEvent> generate_synthetic_mbo(const SyntheticConfig& cfg) {
    std::mt19937_64 rng(cfg.seed);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::uniform_int_distribution<int> level_dist(0, cfg.levels_each_side - 1);
    std::uniform_int_distribution<int> qty_dist(static_cast<int>(cfg.min_qty),
                                                static_cast<int>(cfg.max_qty));
    std::bernoulli_distribution side_dist(0.5);

    struct Live {
        OrderId id;
        Side side;
        Price price;
        Qty qty;
    };
    std::vector<Live> live;
    live.reserve(4096);

    std::vector<BookEvent> events;
    events.reserve(cfg.num_events);
    OrderId next_id = 1;
    Timestamp ts = 1;

    auto price_for = [&](Side side, int level) -> Price {
        if (side == Side::Buy) {
            return cfg.mid - cfg.tick * (1 + level);
        }
        return cfg.mid + cfg.tick * (1 + level);
    };

    // Seed book with resting liquidity
    for (int lvl = 0; lvl < cfg.levels_each_side; ++lvl) {
        for (int k = 0; k < 5; ++k) {
            for (Side side : {Side::Buy, Side::Sell}) {
                BookEvent e;
                e.type = EventType::Add;
                e.order_id = next_id++;
                e.side = side;
                e.price = price_for(side, lvl);
                e.qty = static_cast<Qty>(qty_dist(rng));
                e.ts = ts++;
                events.push_back(e);
                live.push_back(Live{e.order_id, e.side, e.price, e.qty});
            }
        }
    }

    while (events.size() < cfg.num_events) {
        const double r = u01(rng);
        BookEvent e{};
        e.ts = ts++;

        if (!live.empty() && r < cfg.cancel_prob) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t idx = pick(rng);
            e.type = EventType::Cancel;
            e.order_id = live[idx].id;
            e.side = live[idx].side;
            e.price = live[idx].price;
            e.qty = 0;
            events.push_back(e);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
            continue;
        }

        if (!live.empty() && r < cfg.cancel_prob + cfg.modify_prob) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            Live& L = live[pick(rng)];
            if (L.qty <= 1) {
                continue;
            }
            std::uniform_int_distribution<int> nq(1, static_cast<int>(L.qty - 1));
            e.type = EventType::Modify;
            e.order_id = L.id;
            e.side = L.side;
            e.price = L.price;
            e.qty = static_cast<Qty>(nq(rng));
            L.qty = e.qty;
            events.push_back(e);
            continue;
        }

        if (!live.empty() && r < cfg.cancel_prob + cfg.modify_prob + cfg.execute_prob) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t idx = pick(rng);
            Live& L = live[idx];
            std::uniform_int_distribution<int> eq(1, static_cast<int>(L.qty));
            const Qty exec = static_cast<Qty>(eq(rng));
            e.type = EventType::Execute;
            e.order_id = L.id;
            e.side = L.side;
            e.price = L.price;
            e.qty = exec;
            events.push_back(e);
            L.qty -= exec;
            if (L.qty == 0) {
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
            }
            continue;
        }

        // Add
        e.type = EventType::Add;
        e.order_id = next_id++;
        e.side = side_dist(rng) ? Side::Buy : Side::Sell;
        e.price = price_for(e.side, level_dist(rng));
        e.qty = static_cast<Qty>(qty_dist(rng));
        events.push_back(e);
        live.push_back(Live{e.order_id, e.side, e.price, e.qty});
    }

    return events;
}

}  // namespace fastlob
