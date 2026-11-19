#pragma once

#include "fastlob/order.hpp"
#include "fastlob/types.hpp"

#include <algorithm>
#include <cstddef>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastlob {

/// Gold-standard L3 book: std::map + std::list. Correctness over speed.
class ReferenceOrderBook {
public:
    bool add(OrderId id, Side side, Price price, Qty qty, Timestamp ts = 0) {
        if (id == kInvalidOrderId || qty == 0 || index_.count(id)) {
            return false;
        }
        auto& level = levels(side)[price];
        level.push_back(Order{id, price, qty, 0, side, {}, nullptr, nullptr, ts});
        auto it = std::prev(level.end());
        index_[id] = Loc{side, price, it};
        return true;
    }

    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) {
            return false;
        }
        erase_loc(it->second);
        index_.erase(it);
        return true;
    }

    /// MVP: qty decrease only (keeps queue priority). new_qty == 0 => cancel.
    bool modify(OrderId id, Qty new_qty) {
        auto it = index_.find(id);
        if (it == index_.end()) {
            return false;
        }
        if (new_qty == 0) {
            erase_loc(it->second);
            index_.erase(it);
            return true;
        }
        if (new_qty > it->second.iter->qty) {
            return false;  // increase not supported in MVP
        }
        it->second.iter->qty = new_qty;
        return true;
    }

    /// Apply trade against resting order; removes order when fully filled.
    bool execute(OrderId id, Qty exec_qty) {
        auto it = index_.find(id);
        if (it == index_.end() || exec_qty == 0) {
            return false;
        }
        Order& o = *it->second.iter;
        if (exec_qty > o.qty) {
            return false;
        }
        o.qty -= exec_qty;
        if (o.qty == 0) {
            erase_loc(it->second);
            index_.erase(it);
        }
        return true;
    }

    bool apply(const BookEvent& e) {
        switch (e.type) {
            case EventType::Add:
                return add(e.order_id, e.side, e.price, e.qty, e.ts);
            case EventType::Cancel:
                return cancel(e.order_id);
            case EventType::Modify:
                return modify(e.order_id, e.qty);
            case EventType::Execute:
                return execute(e.order_id, e.qty);
        }
        return false;
    }

    [[nodiscard]] std::optional<Order> find(OrderId id) const {
        auto it = index_.find(id);
        if (it == index_.end()) {
            return std::nullopt;
        }
        return *it->second.iter;
    }

    [[nodiscard]] Qty level_qty(Side side, Price price) const {
        const auto& m = levels_c(side);
        auto it = m.find(price);
        if (it == m.end()) {
            return 0;
        }
        Qty sum = 0;
        for (const auto& o : it->second) {
            sum += o.qty;
        }
        return sum;
    }

    [[nodiscard]] std::size_t order_count() const { return index_.size(); }

    [[nodiscard]] std::size_t top_n(Side side, std::span<LevelView> out) const {
        const auto& m = levels_c(side);
        std::size_t n = 0;
        if (side == Side::Buy) {
            for (auto it = m.rbegin(); it != m.rend() && n < out.size(); ++it) {
                Qty q = 0;
                for (const auto& o : it->second) {
                    q += o.qty;
                }
                if (q == 0) {
                    continue;
                }
                out[n++] = LevelView{it->first, q};
            }
        } else {
            for (auto it = m.begin(); it != m.end() && n < out.size(); ++it) {
                Qty q = 0;
                for (const auto& o : it->second) {
                    q += o.qty;
                }
                if (q == 0) {
                    continue;
                }
                out[n++] = LevelView{it->first, q};
            }
        }
        return n;
    }

    /// Iterate resting order ids at a price in FIFO order (for assertions).
    [[nodiscard]] std::vector<OrderId> fifo_ids(Side side, Price price) const {
        std::vector<OrderId> ids;
        const auto& m = levels_c(side);
        auto it = m.find(price);
        if (it == m.end()) {
            return ids;
        }
        for (const auto& o : it->second) {
            ids.push_back(o.order_id);
        }
        return ids;
    }

    void clear() {
        bids_.clear();
        asks_.clear();
        index_.clear();
    }

private:
    using LevelList = std::list<Order>;
    using BidMap = std::map<Price, LevelList>;  // ascending; best bid = rbegin
    using AskMap = std::map<Price, LevelList>;  // ascending; best ask = begin

    struct Loc {
        Side side;
        Price price;
        LevelList::iterator iter;
    };

    BidMap& levels(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const BidMap& levels_c(Side side) const { return side == Side::Buy ? bids_ : asks_; }

    void erase_loc(const Loc& loc) {
        auto& m = levels(loc.side);
        auto lit = m.find(loc.price);
        if (lit == m.end()) {
            return;
        }
        lit->second.erase(loc.iter);
        if (lit->second.empty()) {
            m.erase(lit);
        }
    }

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Loc> index_;
};

}  // namespace fastlob
