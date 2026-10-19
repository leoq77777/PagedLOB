#pragma once

#include "fastlob/memory_pool.hpp"
#include "fastlob/order.hpp"
#include "fastlob/types.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fastlob {

struct PriceLevel {
    Price price{0};
    Qty total_qty{0};
    Side side{Side::Buy};
    Order* head{nullptr};
    Order* tail{nullptr};
    PriceLevel* better{nullptr};  // toward BBO
    PriceLevel* worse{nullptr};   // away from BBO
    bool active{false};
};

/// Open-addressing Price -> PriceLevel* (fixed capacity).
class LevelIndex {
public:
    explicit LevelIndex(std::size_t capacity)
        : capacity_(next_pow2(capacity < 16 ? 16 : capacity)),
          mask_(capacity_ - 1),
          keys_(capacity_, kInvalidPrice),
          vals_(capacity_, nullptr),
          size_(0) {}

    // returns true if newly inserted
    bool insert(Price px, PriceLevel* lvl) {
        assert(size_ < capacity_);
        std::size_t i = hash(px) & mask_;
        for (;;) {
            if (keys_[i] == kInvalidPrice) {
                keys_[i] = px;
                vals_[i] = lvl;
                ++size_;
                return true;
            }
            if (keys_[i] == px) {
                vals_[i] = lvl;
                return false;
            }
            i = (i + 1) & mask_;
        }
    }

    PriceLevel* find(Price px) const {
        std::size_t i = hash(px) & mask_;
        for (;;) {
            const Price k = keys_[i];
            if (k == kInvalidPrice) {
                return nullptr;
            }
            if (k == px) {
                return vals_[i];
            }
            i = (i + 1) & mask_;
        }
    }

    bool erase(Price px) {
        std::size_t i = hash(px) & mask_;
        for (;;) {
            const Price k = keys_[i];
            if (k == kInvalidPrice) {
                return false;
            }
            if (k == px) {
                keys_[i] = kInvalidPrice;
                vals_[i] = nullptr;
                --size_;
                rehash_from((i + 1) & mask_);
                return true;
            }
            i = (i + 1) & mask_;
        }
    }

private:
    static std::size_t next_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    static std::size_t hash(Price px) {
        std::uint64_t x = static_cast<std::uint64_t>(px);
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        return static_cast<std::size_t>(x);
    }

    void rehash_from(std::size_t start) {
        std::size_t i = start;
        for (;;) {
            const Price k = keys_[i];
            if (k == kInvalidPrice) {
                return;
            }
            PriceLevel* v = vals_[i];
            keys_[i] = kInvalidPrice;
            vals_[i] = nullptr;
            --size_;
            insert(k, v);
            i = (i + 1) & mask_;
        }
    }

    std::size_t capacity_;
    std::size_t mask_;
    std::vector<Price> keys_;
    std::vector<PriceLevel*> vals_;
    std::size_t size_;
};

/// Fast L3 book: order pool + intrusive FIFO + fixed OrderId / price indexes.
/// After construction, add/cancel/modify/execute do not heap-allocate.
class FastOrderBook {
public:
    explicit FastOrderBook(std::size_t max_orders = 1 << 20, std::size_t max_levels = 1 << 16)
        : pool_(max_orders),
          index_(max_orders * 2),
          level_storage_(max_levels),
          bid_levels_(max_levels * 2),
          ask_levels_(max_levels * 2),
          free_levels_(nullptr),
          best_bid_(nullptr),
          best_ask_(nullptr),
          order_count_(0) {
        for (std::size_t i = 0; i < max_levels; ++i) {
            PriceLevel* lvl = &level_storage_[i];
            lvl->better = free_levels_;
            free_levels_ = lvl;
        }
    }

    FastOrderBook(const FastOrderBook&) = delete;
    FastOrderBook& operator=(const FastOrderBook&) = delete;

    bool add(OrderId id, Side side, Price price, Qty qty, Timestamp ts = 0) {
        if (id == kInvalidOrderId || qty == 0 || index_.find(id) != nullptr) {
            return false;
        }
        Order* o = pool_.allocate();
        o->order_id = id;
        o->price = price;
        o->qty = qty;
        o->side = side;
        o->timestamp = ts;
        o->prev = nullptr;
        o->next = nullptr;

        PriceLevel* lvl = get_or_create_level(side, price);
        push_back(lvl, o);
        index_.insert(id, o);
        ++order_count_;
        return true;
    }

    bool cancel(OrderId id) {
        Order* o = index_.find(id);
        if (o == nullptr) {
            return false;
        }
        remove_order(o);
        return true;
    }

    bool modify(OrderId id, Qty new_qty) {
        Order* o = index_.find(id);
        if (o == nullptr) {
            return false;
        }
        if (new_qty == 0) {
            remove_order(o);
            return true;
        }
        if (new_qty > o->qty) {
            return false;
        }
        PriceLevel* lvl = level_index(o->side).find(o->price);
        assert(lvl != nullptr);
        lvl->total_qty -= (o->qty - new_qty);
        o->qty = new_qty;
        return true;
    }

    bool execute(OrderId id, Qty exec_qty) {
        Order* o = index_.find(id);
        if (o == nullptr || exec_qty == 0 || exec_qty > o->qty) {
            return false;
        }
        const Side side = o->side;
        PriceLevel* lvl = level_index(side).find(o->price);
        assert(lvl != nullptr);
        o->qty -= exec_qty;
        lvl->total_qty -= exec_qty;
        if (o->qty == 0) {
            unlink(lvl, o);
            index_.erase(id);
            pool_.deallocate(o);
            --order_count_;
            if (lvl->head == nullptr) {
                destroy_level(side, lvl);
            }
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
        Order* o = index_.find(id);
        if (o == nullptr) {
            return std::nullopt;
        }
        return *o;
    }

    [[nodiscard]] Qty level_qty(Side side, Price price) const {
        PriceLevel* lvl = level_index_c(side).find(price);
        return lvl ? lvl->total_qty : 0;
    }

    [[nodiscard]] std::size_t order_count() const { return order_count_; }

    [[nodiscard]] std::size_t top_n(Side side, std::span<LevelView> out) const {
        std::size_t n = 0;
        PriceLevel* cur = (side == Side::Buy) ? best_bid_ : best_ask_;
        while (cur != nullptr && n < out.size()) {
            if (cur->total_qty > 0) {
                out[n++] = LevelView{cur->price, cur->total_qty};
            }
            cur = cur->worse;
        }
        return n;
    }

    [[nodiscard]] std::vector<OrderId> fifo_ids(Side side, Price price) const {
        std::vector<OrderId> ids;
        PriceLevel* lvl = level_index_c(side).find(price);
        if (lvl == nullptr) {
            return ids;
        }
        for (Order* o = lvl->head; o != nullptr; o = o->next) {
            ids.push_back(o->order_id);
        }
        return ids;
    }

    /// Contiguous snapshot for Python buffer export: [bid_px, bid_qty, ask_px, ask_qty] * n
    [[nodiscard]] std::size_t snapshot_top_n(std::size_t n, std::span<std::int64_t> out) const {
        // out layout: for i in [0,n): bid_px, bid_qty, ask_px, ask_qty  => 4*n ints
        if (out.size() < n * 4) {
            return 0;
        }
        LevelView bids[64];
        LevelView asks[64];
        const std::size_t nb = top_n(Side::Buy, std::span<LevelView>(bids, std::min(n, std::size_t{64})));
        const std::size_t na = top_n(Side::Sell, std::span<LevelView>(asks, std::min(n, std::size_t{64})));
        for (std::size_t i = 0; i < n; ++i) {
            out[i * 4 + 0] = (i < nb) ? bids[i].price : 0;
            out[i * 4 + 1] = (i < nb) ? static_cast<std::int64_t>(bids[i].qty) : 0;
            out[i * 4 + 2] = (i < na) ? asks[i].price : 0;
            out[i * 4 + 3] = (i < na) ? static_cast<std::int64_t>(asks[i].qty) : 0;
        }
        return n;
    }

private:
    LevelIndex& level_index(Side side) { return side == Side::Buy ? bid_levels_ : ask_levels_; }
    const LevelIndex& level_index_c(Side side) const {
        return side == Side::Buy ? bid_levels_ : ask_levels_;
    }

    PriceLevel*& best_ref(Side side) { return side == Side::Buy ? best_bid_ : best_ask_; }

    bool is_better(Side side, Price a, Price b) const {
        return side == Side::Buy ? a > b : a < b;
    }

    PriceLevel* get_or_create_level(Side side, Price price) {
        if (PriceLevel* existing = level_index(side).find(price)) {
            return existing;
        }
        assert(free_levels_ != nullptr && "level pool exhausted");
        PriceLevel* lvl = free_levels_;
        free_levels_ = free_levels_->better;
        lvl->price = price;
        lvl->total_qty = 0;
        lvl->side = side;
        lvl->head = nullptr;
        lvl->tail = nullptr;
        lvl->better = nullptr;
        lvl->worse = nullptr;
        lvl->active = true;
        insert_level_sorted(side, lvl);
        level_index(side).insert(price, lvl);
        return lvl;
    }

    void insert_level_sorted(Side side, PriceLevel* lvl) {
        PriceLevel*& best = best_ref(side);
        if (best == nullptr) {
            best = lvl;
            return;
        }
        if (is_better(side, lvl->price, best->price)) {
            lvl->worse = best;
            best->better = lvl;
            best = lvl;
            return;
        }
        PriceLevel* cur = best;
        while (cur->worse != nullptr && !is_better(side, lvl->price, cur->worse->price)) {
            cur = cur->worse;
        }
        // insert after cur (lvl is worse than cur)
        lvl->better = cur;
        lvl->worse = cur->worse;
        if (cur->worse) {
            cur->worse->better = lvl;
        }
        cur->worse = lvl;
    }

    void destroy_level(Side side, PriceLevel* lvl) {
        if (lvl->better) {
            lvl->better->worse = lvl->worse;
        } else {
            best_ref(side) = lvl->worse;
        }
        if (lvl->worse) {
            lvl->worse->better = lvl->better;
        }
        level_index(side).erase(lvl->price);
        lvl->active = false;
        lvl->head = lvl->tail = nullptr;
        lvl->better = free_levels_;
        lvl->worse = nullptr;
        free_levels_ = lvl;
    }

    void push_back(PriceLevel* lvl, Order* o) {
        o->prev = lvl->tail;
        o->next = nullptr;
        if (lvl->tail) {
            lvl->tail->next = o;
        } else {
            lvl->head = o;
        }
        lvl->tail = o;
        lvl->total_qty += o->qty;
    }

    void unlink(PriceLevel* lvl, Order* o) {
        if (o->prev) {
            o->prev->next = o->next;
        } else {
            lvl->head = o->next;
        }
        if (o->next) {
            o->next->prev = o->prev;
        } else {
            lvl->tail = o->prev;
        }
        o->prev = o->next = nullptr;
    }

    void remove_order(Order* o) {
        const Side side = o->side;
        const Price price = o->price;
        const OrderId id = o->order_id;
        PriceLevel* lvl = level_index(side).find(price);
        assert(lvl != nullptr);
        lvl->total_qty -= o->qty;
        unlink(lvl, o);
        index_.erase(id);
        pool_.deallocate(o);
        --order_count_;
        if (lvl->head == nullptr) {
            destroy_level(side, lvl);
        }
    }

    OrderPool pool_;
    OrderIdIndex index_;
    std::vector<PriceLevel> level_storage_;
    LevelIndex bid_levels_;
    LevelIndex ask_levels_;
    PriceLevel* free_levels_;
    PriceLevel* best_bid_;
    PriceLevel* best_ask_;
    std::size_t order_count_;
};

}  // namespace fastlob
