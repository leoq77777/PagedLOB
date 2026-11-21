#pragma once

// Shared order path for the std::map price index and the paged directory.
//
// What is shared, as types and as the add/cancel/execute implementation:
//   - fastlob::Order, OrderPool, and OrderIdIndex
//     (intrusive Order* FIFO, contiguous open-addressed OrderId → Order*)
//   - BookLevel / LevelPool (32-bit level handles, 64-bit aggregate quantity)
//
// What is not shared:
//   - each book owns its own pools (nothing is shared at runtime)
//   - FastOrderBook (v1) is a separate type and is not modified
//   - the price index: MapPriceIndex versus PagedPriceIndex
//
// Each Order stores a 32-bit LevelPool handle so cancel/execute/match do not
// look the price up again.

#include "fastlob/map_price_index.hpp"
#include "fastlob/memory_pool.hpp"
#include "fastlob/paged_price_index.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace fastlob {

struct BookMemory {
    std::size_t order_bytes{0};
    std::size_t id_bytes{0};
    std::size_t level_bytes{0};
    /// Retained price-index bytes. Paged includes its free-list reserve.
    /// Map is the bytes its nodes currently occupy (erase returns them).
    std::size_t price_bytes{0};
    std::size_t price_live_bytes{0};
    std::size_t price_peak_bytes{0};

    [[nodiscard]] std::size_t total_bytes() const {
        return order_bytes + id_bytes + level_bytes + price_bytes;
    }
};

template <typename PriceIndex>
class StorageBook {
public:
    StorageBook(std::size_t max_orders, std::size_t max_levels)
        : pool_(max_orders),
          ids_(max_orders * 2),
          levels_(max_levels),
          bids_(levels_, Side::Buy),
          asks_(levels_, Side::Sell) {}

    StorageBook(const StorageBook&) = delete;
    StorageBook& operator=(const StorageBook&) = delete;

    /// Pre-allocate cold directory nodes and leaves on each side for zero-alloc cold inserts.
    void reserve_cold(std::size_t nodes_per_side, std::size_t leaves_per_side) {
        bids_.reserve(nodes_per_side, leaves_per_side);
        asks_.reserve(nodes_per_side, leaves_per_side);
    }

    bool add(OrderId id, Side side, Price price, Qty qty, Timestamp ts = 0) {
        if (id == kInvalidOrderId || qty == 0 || pool_.free_count() == 0) {
            return false;
        }
        std::size_t id_slot = 0;
        if (!ids_.find_empty_slot(id, id_slot)) {
            return false;
        }
        return insert_at_slot(id_slot, id, side, price, qty, ts);
    }

    /// Cross the opposite side at the maker price, then rest any remainder.
    /// Does not run inside apply(); MBO replay stays event-faithful.
    MatchResult match_limit(OrderId id, Side side, Price price, Qty qty, Timestamp ts = 0,
                            std::vector<Fill>* fills = nullptr) {
        return match_incoming(id, side, price, qty, /*market=*/false, ts, fills);
    }

    /// Walk the opposite side until qty is gone or the book is empty. Remainder does not rest.
    MatchResult match_market(OrderId id, Side side, Qty qty, Timestamp ts = 0,
                             std::vector<Fill>* fills = nullptr) {
        const Price cap = side == Side::Buy ? std::numeric_limits<Price>::max()
                                            : std::numeric_limits<Price>::min();
        return match_incoming(id, side, cap, qty, /*market=*/true, ts, fills);
    }

    bool cancel(OrderId id) {
        Order* order = ids_.take(id);
        if (order == nullptr) {
            return false;
        }
        unlink_and_free(order);
        return true;
    }

    /// Qty decrease only. new_qty == 0 cancels. An increase is rejected.
    bool modify(OrderId id, Qty new_qty) {
        std::size_t slot = 0;
        Order* order = ids_.find_with_slot(id, slot);
        if (order == nullptr) {
            return false;
        }
        if (new_qty == 0) {
            ids_.erase_slot(slot);
            unlink_and_free(order);
            return true;
        }
        if (new_qty > order->qty) {
            return false;
        }
        BookLevel* level = level_of(order);
        level->total_qty -= static_cast<std::uint64_t>(order->qty - new_qty);
        order->qty = new_qty;
        return true;
    }

    bool execute(OrderId id, Qty exec_qty) {
        std::size_t slot = 0;
        Order* order = ids_.find_with_slot(id, slot);
        if (order == nullptr || exec_qty == 0 || exec_qty > order->qty) {
            return false;
        }
        BookLevel* level = level_of(order);
        order->qty -= exec_qty;
        level->total_qty -= exec_qty;
        if (order->qty == 0) {
            ids_.erase_slot(slot);
            finish_depleted_order(level, order);
        }
        return true;
    }

    bool apply(const BookEvent& event) {
        switch (event.type) {
            case EventType::Add:
                return add(event.order_id, event.side, event.price, event.qty, event.ts);
            case EventType::Cancel:
                return cancel(event.order_id);
            case EventType::Modify:
                return modify(event.order_id, event.qty);
            case EventType::Execute:
                return execute(event.order_id, event.qty);
        }
        return false;
    }

    /// Returns a value snapshot of the order with internal pool links/handles scrubbed.
    [[nodiscard]] std::optional<Order> find(OrderId id) const {
        Order* order = ids_.find(id);
        if (order == nullptr) {
            return std::nullopt;
        }
        Order snapshot = *order;
        snapshot.prev = nullptr;
        snapshot.next = nullptr;
        snapshot.level_handle = 0;
        return snapshot;
    }

    [[nodiscard]] LevelQty level_qty(Side side, Price price) const {
        const BookLevel* level = side_index(side).find(price);
        return level == nullptr ? 0 : level->total_qty;
    }

    [[nodiscard]] std::size_t order_count() const { return order_count_; }

    [[nodiscard]] std::size_t active_levels(Side side) const { return side_index(side).active(); }

    [[nodiscard]] std::size_t top_n(Side side, std::span<LevelView> out) const {
        return side_index(side).copy_top(out);
    }

    /// Contiguous snapshot for Python buffer export: [bid_px, bid_qty, ask_px, ask_qty] * n
    [[nodiscard]] std::size_t snapshot_top_n(std::size_t n, std::span<std::int64_t> out) const {
        if (out.size() < n * 4) {
            return 0;
        }
        LevelView stack_bids[64];
        LevelView stack_asks[64];
        std::vector<LevelView> heap_bids;
        std::vector<LevelView> heap_asks;
        std::span<LevelView> bids_span;
        std::span<LevelView> asks_span;
        if (n <= 64) {
            bids_span = std::span<LevelView>(stack_bids, n);
            asks_span = std::span<LevelView>(stack_asks, n);
        } else {
            heap_bids.resize(n);
            heap_asks.resize(n);
            bids_span = heap_bids;
            asks_span = heap_asks;
        }
        const std::size_t nb = top_n(Side::Buy, bids_span);
        const std::size_t na = top_n(Side::Sell, asks_span);
        for (std::size_t i = 0; i < n; ++i) {
            out[i * 4 + 0] = (i < nb) ? bids_span[i].price : 0;
            out[i * 4 + 1] = (i < nb) ? static_cast<std::int64_t>(bids_span[i].qty) : 0;
            out[i * 4 + 2] = (i < na) ? asks_span[i].price : 0;
            out[i * 4 + 3] = (i < na) ? static_cast<std::int64_t>(asks_span[i].qty) : 0;
        }
        return n;
    }

    [[nodiscard]] std::vector<OrderId> fifo_ids(Side side, Price price) const {
        std::vector<OrderId> ids;
        const BookLevel* level = side_index(side).find(price);
        if (level == nullptr) {
            return ids;
        }
        for (const Order* order = level->head; order != nullptr; order = order->next) {
            ids.push_back(order->order_id);
        }
        return ids;
    }

    [[nodiscard]] BookMemory memory() const {
        BookMemory stats;
        stats.order_bytes = pool_.capacity() * sizeof(Order);
        stats.id_bytes = ids_.bytes();
        stats.level_bytes = levels_.bytes();
        stats.price_bytes = bids_.bytes() + asks_.bytes();
        stats.price_live_bytes = bids_.live_bytes() + asks_.live_bytes();
        stats.price_peak_bytes = bids_.peak_bytes() + asks_.peak_bytes();
        return stats;
    }

    [[nodiscard]] PageStats page_stats() const { return bids_.stats() + asks_.stats(); }

    [[nodiscard]] std::optional<Price> best_price(Side side) const {
        const BookLevel* level = side_index(side).best();
        if (level == nullptr) {
            return std::nullopt;
        }
        return level->price;
    }

private:
    static bool crosses(Side taker, Price taker_px, Price maker_px) {
        return taker == Side::Buy ? taker_px >= maker_px : taker_px <= maker_px;
    }

    [[nodiscard]] BookLevel* level_of(Order* order) {
        assert(order != nullptr && order->level_handle != 0);
        return levels_.get(order->level_handle);
    }

    bool insert_at_slot(std::size_t id_slot, OrderId id, Side side, Price price, Qty qty, Timestamp ts) {
        BookLevel* level = side_index(side).find_or_open(price);
        if (level == nullptr) {
            return false;
        }
        Order* order = pool_.allocate();
        assert(order != nullptr);
        order->order_id = id;
        order->price = price;
        order->qty = qty;
        order->side = side;
        order->timestamp = ts;
        order->level_handle = levels_.handle_of(level);
        order->prev = nullptr;
        order->next = nullptr;
        fifo_push(*level, order);
        ids_.commit_slot(id_slot, id, order);
        ++order_count_;
        return true;
    }

    void finish_depleted_order(BookLevel* level, Order* order) {
        const Side side = order->side;
        fifo_unlink(*level, order);
        pool_.deallocate(order);
        --order_count_;
        if (level->head == nullptr) {
            side_index(side).close(level);
        }
    }

    void unlink_and_free(Order* order) {
        BookLevel* level = level_of(order);
        assert(level->total_qty >= order->qty);
        level->total_qty -= order->qty;
        finish_depleted_order(level, order);
    }

    void apply_maker_fill(BookLevel* level, Order* maker, Qty take) {
        assert(maker->qty >= take);
        assert(level->total_qty >= take);
        maker->qty -= take;
        level->total_qty -= take;
        if (maker->qty == 0) {
            ids_.erase(maker->order_id);
            finish_depleted_order(level, maker);
        }
    }

    MatchResult match_incoming(OrderId id, Side side, Price price, Qty qty, bool market, Timestamp ts,
                               std::vector<Fill>* fills) {
        MatchResult result;
        if (id == kInvalidOrderId || qty == 0) {
            return result;
        }
        std::size_t id_slot = 0;
        if (!ids_.find_empty_slot(id, id_slot)) {
            return result;
        }
        result.ok = true;
        result.remaining = qty;
        PriceIndex& opposite = side_index(side == Side::Buy ? Side::Sell : Side::Buy);
        while (result.remaining > 0) {
            BookLevel* level = opposite.best();
            if (level == nullptr) {
                break;
            }
            if (!market && !crosses(side, price, level->price)) {
                break;
            }
            Order* maker = level->head;
            assert(maker != nullptr);
            const Qty take = std::min(result.remaining, maker->qty);
            if (fills != nullptr) {
                fills->push_back(Fill{maker->order_id, id, level->price, take});
            }
            result.filled += take;
            result.remaining -= take;
            apply_maker_fill(level, maker, take);
        }
        if (result.remaining > 0 && !market) {
            if (result.filled == 0) {
                if (pool_.free_count() == 0) {
                    result.ok = false;
                    return result;
                }
                result.rested = insert_at_slot(id_slot, id, side, price, result.remaining, ts);
                result.ok = result.rested;
            } else {
                // Draining at least one crossing best level freed >=1 Order, >=1 BookLevel,
                // and >=1 OrderIdIndex slot, so resting the remainder cannot fail capacity.
                result.rested = add(id, side, price, result.remaining, ts);
                assert(result.rested);
                result.ok = result.rested;
            }
        }
        return result;
    }

    PriceIndex& side_index(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const PriceIndex& side_index(Side side) const { return side == Side::Buy ? bids_ : asks_; }

    OrderPool pool_;
    OrderIdIndex ids_;
    LevelPool levels_;
    PriceIndex bids_;
    PriceIndex asks_;
    std::size_t order_count_{0};
};

using MapStorageBook = StorageBook<MapPriceIndex>;
template <int PageBits = kDefaultPageBits, int HotTicks = 256>
using PagedStorageBookT = StorageBook<PagedPriceIndex<PageBits, HotTicks>>;
using PagedStorageBook = PagedStorageBookT<kDefaultPageBits, 256>;
/// Public name of the paged engine. Same type as PagedStorageBook (256-tick pages, 256-tick hot ring).
using PagedLOB = PagedStorageBook;

}  // namespace fastlob
