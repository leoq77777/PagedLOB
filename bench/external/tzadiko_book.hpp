#pragma once

// Tzadiko's book matches inside AddOrder and takes a mutex on every call.
// A background thread waits to prune GoodForDay orders; these benchmarks
// only submit GoodTillCancel, so that thread should stay asleep.

#include "Orderbook.h"

#include <cstdint>
#include <vector>

class TzadikoBook {
public:
    TzadikoBook(std::size_t, std::size_t) {}

    bool add(std::uint64_t id, bool buy, std::int64_t price, std::uint32_t qty) {
        if (price <= 0 || qty == 0) {
            return false;
        }
        book_.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, id, buy ? Side::Buy : Side::Sell,
                                               static_cast<Price>(price), qty));
        return true;
    }

    bool match(std::uint64_t id, bool buy, std::int64_t price, std::uint32_t qty) {
        return add(id, buy, price, qty);
    }

    bool cancel(std::uint64_t id) {
        book_.CancelOrder(id);
        return true;
    }

    std::uint64_t sweep_bids() const {
        const OrderbookLevelInfos infos = book_.GetOrderInfos();
        std::uint64_t sink = 0;
        for (const LevelInfo& level : infos.GetBids()) {
            sink += level.quantity_;
            sink += static_cast<std::uint64_t>(level.price_);
        }
        return sink;
    }

    struct Level {
        std::int64_t price;
        std::uint64_t qty;
    };

    void collect(bool buy, std::vector<Level>& out) const {
        const OrderbookLevelInfos infos = book_.GetOrderInfos();
        const LevelInfos& side = buy ? infos.GetBids() : infos.GetAsks();
        out.clear();
        out.reserve(side.size());
        for (const LevelInfo& level : side) {
            out.push_back(Level{level.price_, level.quantity_});
        }
    }

private:
    Orderbook book_;
};
