#pragma once

// Shared order path for the std::map price index and the paged directory.
//
// What is shared, as types and as the add/cancel/execute implementation:
//   - fastlob::Order, OrderPool, and OrderIdIndex from the v1 baseline
//     (intrusive Order* FIFO, open-addressed OrderId → Order*)
//   - BookLevel / LevelPool (32-bit level handles, 64-bit aggregate quantity)
//
// What is not shared:
//   - each book owns its own pools (nothing is shared at runtime)
//   - FastOrderBook (v1) is a separate type and is not modified
//   - the price index: MapPriceIndex versus PagedPriceIndex
//
// Order objects stay pointer-linked like v1. The proposal's 32-bit order
// handles belong with elastic slabs and are not part of this step. Level
// identity inside a price index is a 32-bit handle into LevelPool.

#include "fastlob/map_price_index.hpp"
#include "fastlob/memory_pool.hpp"
#include "fastlob/paged_price_index.hpp"

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

    bool add(OrderId id, Side side, Price price, Qty qty, Timestamp ts = 0) {
        if (id == kInvalidOrderId || qty == 0 || ids_.find(id) != nullptr) {
            return false;
        }
        PriceIndex& index = side_index(side);
        BookLevel* level = index.find(price);
        const bool created = level == nullptr;
        if (created) {
            level = index.open(price);
            if (level == nullptr) {
                return false;
            }
        }
        if (pool_.free_count() == 0) {
            if (created) {
                index.close(level);
            }
            return false;
        }
        Order* order = pool_.allocate();
        order->order_id = id;
        order->price = price;
        order->qty = qty;
        order->side = side;
        order->timestamp = ts;
        order->prev = nullptr;
        order->next = nullptr;
        fifo_push(*level, order);
        ids_.insert(id, order);
        ++order_count_;
        return true;
    }

    bool cancel(OrderId id) {
        Order* order = ids_.find(id);
        if (order == nullptr) {
            return false;
        }
        remove_order(order);
        return true;
    }

    /// Qty decrease only. new_qty == 0 cancels. An increase is rejected.
    bool modify(OrderId id, Qty new_qty) {
        Order* order = ids_.find(id);
        if (order == nullptr) {
            return false;
        }
        if (new_qty == 0) {
            remove_order(order);
            return true;
        }
        if (new_qty > order->qty) {
            return false;
        }
        BookLevel* level = side_index(order->side).find(order->price);
        assert(level != nullptr);
        level->total_qty -= static_cast<std::uint64_t>(order->qty - new_qty);
        order->qty = new_qty;
        return true;
    }

    bool execute(OrderId id, Qty exec_qty) {
        Order* order = ids_.find(id);
        if (order == nullptr || exec_qty == 0 || exec_qty > order->qty) {
            return false;
        }
        const Side side = order->side;
        BookLevel* level = side_index(side).find(order->price);
        assert(level != nullptr);
        order->qty -= exec_qty;
        level->total_qty -= exec_qty;
        if (order->qty == 0) {
            fifo_unlink(*level, order);
            ids_.erase(id);
            pool_.deallocate(order);
            --order_count_;
            if (level->head == nullptr) {
                side_index(side).close(level);
            }
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

    [[nodiscard]] std::optional<Order> find(OrderId id) const {
        Order* order = ids_.find(id);
        if (order == nullptr) {
            return std::nullopt;
        }
        return *order;
    }

    [[nodiscard]] Qty level_qty(Side side, Price price) const {
        const BookLevel* level = side_index(side).find(price);
        if (level == nullptr) {
            return 0;
        }
        assert(level->total_qty <= std::numeric_limits<Qty>::max());
        return static_cast<Qty>(level->total_qty);
    }

    [[nodiscard]] std::size_t order_count() const { return order_count_; }

    [[nodiscard]] std::size_t active_levels(Side side) const { return side_index(side).active(); }

    [[nodiscard]] std::size_t top_n(Side side, std::span<LevelView> out) const {
        return side_index(side).copy_top(out);
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
        stats.id_bytes = ids_.capacity() * (sizeof(OrderId) + sizeof(Order*));
        stats.level_bytes = levels_.bytes();
        stats.price_bytes = bids_.bytes() + asks_.bytes();
        stats.price_live_bytes = bids_.live_bytes() + asks_.live_bytes();
        stats.price_peak_bytes = bids_.peak_bytes() + asks_.peak_bytes();
        return stats;
    }

    [[nodiscard]] PageStats page_stats() const { return bids_.stats() + asks_.stats(); }

private:
    void remove_order(Order* order) {
        const Side side = order->side;
        const Price price = order->price;
        const OrderId id = order->order_id;
        const Qty qty = order->qty;
        BookLevel* level = side_index(side).find(price);
        assert(level != nullptr);
        assert(level->total_qty >= qty);
        level->total_qty -= qty;
        fifo_unlink(*level, order);
        ids_.erase(id);
        pool_.deallocate(order);
        --order_count_;
        if (level->head == nullptr) {
            side_index(side).close(level);
        }
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
template <int PageBits = kDefaultPageBits>
using PagedStorageBookT = StorageBook<PagedPriceIndex<PageBits>>;
using PagedStorageBook = PagedStorageBookT<kDefaultPageBits>;
/// Public name of the paged engine. Same type as PagedStorageBook (256-tick pages).
using PagedLOB = PagedStorageBook;

}  // namespace fastlob
