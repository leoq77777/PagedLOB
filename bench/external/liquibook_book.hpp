#pragma once

// Liquibook matches on add. These workloads stay on one side, or keep bids
// strictly below asks, so nothing crosses. Price 0 is their market-order sentinel.

#include <book/order_book.h>
#include <simple/simple_order.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class LiquibookBook {
public:
    LiquibookBook(std::size_t, std::size_t) {}

    bool add(std::uint64_t id, bool buy, std::int64_t price, std::uint32_t qty) {
        if (price <= 0 || qty == 0 || orders_.count(id) != 0) {
            return false;
        }
        auto owned = std::make_unique<liquibook::simple::SimpleOrder>(
            buy, static_cast<liquibook::book::Price>(price), qty);
        liquibook::simple::SimpleOrder* raw = owned.get();
        orders_.emplace(id, std::move(owned));
        book_.add(raw);
        return true;
    }

    bool cancel(std::uint64_t id) {
        const auto it = orders_.find(id);
        if (it == orders_.end()) {
            return false;
        }
        book_.cancel(it->second.get());
        orders_.erase(it);
        return true;
    }

    std::uint64_t sweep_bids() const {
        std::uint64_t sink = 0;
        for (const auto& entry : book_.bids()) {
            sink += entry.second.open_qty();
            sink += entry.first.price();
        }
        return sink;
    }

    struct Level {
        std::int64_t price;
        std::uint64_t qty;
    };

    void collect(bool buy, std::vector<Level>& out) const {
        out.clear();
        const auto& side = buy ? book_.bids() : book_.asks();
        for (const auto& entry : side) {
            const auto price = static_cast<std::int64_t>(entry.first.price());
            if (!out.empty() && out.back().price == price) {
                out.back().qty += entry.second.open_qty();
            } else {
                out.push_back(Level{price, entry.second.open_qty()});
            }
        }
    }

private:
    liquibook::book::OrderBook<liquibook::simple::SimpleOrder*> book_;
    std::unordered_map<std::uint64_t, std::unique_ptr<liquibook::simple::SimpleOrder>> orders_;
};
