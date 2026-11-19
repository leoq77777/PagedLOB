#pragma once

#include "fastlob/order.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace fastlob {

/// Fixed-capacity free-list pool for Order nodes. No heap traffic after construct.
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity)
        : storage_(capacity), free_head_(nullptr), free_count_(capacity) {
        for (std::size_t i = 0; i < capacity; ++i) {
            Order* o = &storage_[i];
            o->next = free_head_;
            free_head_ = o;
        }
    }

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    [[nodiscard]] Order* allocate() {
        assert(free_head_ != nullptr && "OrderPool exhausted");
        Order* o = free_head_;
        free_head_ = o->next;
        --free_count_;
        o->prev = nullptr;
        o->next = nullptr;
        o->level_handle = 0;
        return o;
    }

    void deallocate(Order* o) {
        assert(o != nullptr);
        o->prev = nullptr;
        o->next = free_head_;
        free_head_ = o;
        ++free_count_;
    }

    [[nodiscard]] std::size_t capacity() const { return storage_.size(); }
    [[nodiscard]] std::size_t free_count() const { return free_count_; }
    [[nodiscard]] std::size_t in_use() const { return storage_.size() - free_count_; }

private:
    std::vector<Order> storage_;
    Order* free_head_;
    std::size_t free_count_;
};

/// Open-addressing map: OrderId -> Order*. Fixed capacity, no rehash after construct.
class OrderIdIndex {
public:
    explicit OrderIdIndex(std::size_t capacity)
        : capacity_(next_pow2(capacity < 16 ? 16 : capacity)),
          mask_(capacity_ - 1),
          keys_(capacity_, kInvalidOrderId),
          vals_(capacity_, nullptr),
          size_(0) {}

    OrderIdIndex(const OrderIdIndex&) = delete;
    OrderIdIndex& operator=(const OrderIdIndex&) = delete;

    bool insert(OrderId id, Order* order) {
        assert(id != kInvalidOrderId);
        assert(size_ < capacity_ && "OrderIdIndex full");
        std::size_t i = hash(id) & mask_;
        for (;;) {
            if (keys_[i] == kInvalidOrderId) {
                keys_[i] = id;
                vals_[i] = order;
                ++size_;
                return true;
            }
            if (keys_[i] == id) {
                vals_[i] = order;
                return false;
            }
            i = (i + 1) & mask_;
        }
    }

    [[nodiscard]] Order* find(OrderId id) const {
        if (id == kInvalidOrderId) {
            return nullptr;
        }
        std::size_t i = hash(id) & mask_;
        for (;;) {
            const OrderId k = keys_[i];
            if (k == kInvalidOrderId) {
                return nullptr;
            }
            if (k == id) {
                return vals_[i];
            }
            i = (i + 1) & mask_;
        }
    }

    bool erase(OrderId id) {
        std::size_t i = hash(id) & mask_;
        for (;;) {
            const OrderId k = keys_[i];
            if (k == kInvalidOrderId) {
                return false;
            }
            if (k == id) {
                keys_[i] = kInvalidOrderId;
                vals_[i] = nullptr;
                --size_;
                rehash_from((i + 1) & mask_);
                return true;
            }
            i = (i + 1) & mask_;
        }
    }

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

private:
    static std::size_t next_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    static std::size_t hash(OrderId id) {
        // SplitMix64-ish mix
        std::uint64_t x = id;
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x);
    }

    void rehash_from(std::size_t start) {
        std::size_t i = start;
        for (;;) {
            const OrderId k = keys_[i];
            if (k == kInvalidOrderId) {
                return;
            }
            Order* v = vals_[i];
            keys_[i] = kInvalidOrderId;
            vals_[i] = nullptr;
            --size_;
            insert(k, v);
            i = (i + 1) & mask_;
        }
    }

    std::size_t capacity_;
    std::size_t mask_;
    std::vector<OrderId> keys_;
    std::vector<Order*> vals_;
    std::size_t size_;
};

}  // namespace fastlob
