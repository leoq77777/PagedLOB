#pragma once

#include "fastlob/order.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
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
        if (free_head_ == nullptr) {
            return nullptr;
        }
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

/// Open-addressing map: OrderId -> Order*. Contiguous key+value entries, fixed
/// capacity, no rehash after construct, single-probe insert/erase helpers, and
/// bounded probing so a full table never loops infinitely in Release builds.
class OrderIdIndex {
public:
    struct Entry {
        OrderId id{kInvalidOrderId};
        Order* order{nullptr};
    };

    explicit OrderIdIndex(std::size_t capacity)
        : capacity_(next_pow2(capacity < 16 ? 16 : capacity)),
          mask_(capacity_ - 1),
          slots_(capacity_),
          size_(0) {}

    OrderIdIndex(const OrderIdIndex&) = delete;
    OrderIdIndex& operator=(const OrderIdIndex&) = delete;

    /// Single-probe lookup for insertion: returns false if `id` is invalid,
    /// already present, or the table is full; otherwise writes the target slot.
    [[nodiscard]] bool find_empty_slot(OrderId id, std::size_t& slot_out) const {
        if (id == kInvalidOrderId || size_ >= capacity_) {
            return false;
        }
        std::size_t i = hash(id) & mask_;
        for (std::size_t step = 0; step < capacity_; ++step) {
            const OrderId k = slots_[i].id;
            if (k == kInvalidOrderId) {
                slot_out = i;
                return true;
            }
            if (k == id) {
                return false;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    void commit_slot(std::size_t slot, OrderId id, Order* order) {
        assert(id != kInvalidOrderId);
        assert(slot < capacity_ && slots_[slot].id == kInvalidOrderId);
        slots_[slot].id = id;
        slots_[slot].order = order;
        ++size_;
    }

    bool insert(OrderId id, Order* order) {
        if (id == kInvalidOrderId) {
            return false;
        }
        std::size_t i = hash(id) & mask_;
        for (std::size_t step = 0; step < capacity_; ++step) {
            const OrderId k = slots_[i].id;
            if (k == kInvalidOrderId) {
                slots_[i].id = id;
                slots_[i].order = order;
                ++size_;
                return true;
            }
            if (k == id) {
                slots_[i].order = order;
                return false;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    [[nodiscard]] Order* find(OrderId id) const {
        if (id == kInvalidOrderId) {
            return nullptr;
        }
        std::size_t i = hash(id) & mask_;
        for (std::size_t step = 0; step < capacity_; ++step) {
            const OrderId k = slots_[i].id;
            if (k == kInvalidOrderId) {
                return nullptr;
            }
            if (k == id) {
                return slots_[i].order;
            }
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    [[nodiscard]] Order* find_with_slot(OrderId id, std::size_t& slot_out) const {
        if (id == kInvalidOrderId) {
            return nullptr;
        }
        std::size_t i = hash(id) & mask_;
        for (std::size_t step = 0; step < capacity_; ++step) {
            const OrderId k = slots_[i].id;
            if (k == kInvalidOrderId) {
                return nullptr;
            }
            if (k == id) {
                slot_out = i;
                return slots_[i].order;
            }
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    /// Single-probe find + erase. Returns the removed Order* or nullptr if absent.
    [[nodiscard]] Order* take(OrderId id) {
        std::size_t slot = 0;
        Order* order = find_with_slot(id, slot);
        if (order == nullptr) {
            return nullptr;
        }
        erase_slot(slot);
        return order;
    }

    void erase_slot(std::size_t slot) {
        assert(slot < capacity_ && slots_[slot].id != kInvalidOrderId);
        slots_[slot] = Entry{};
        --size_;
        backshift_from(slot);
    }

    bool erase(OrderId id) { return take(id) != nullptr; }

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    [[nodiscard]] std::size_t bytes() const { return capacity_ * sizeof(Entry); }

private:
    static std::size_t next_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    static std::size_t hash(OrderId id) {
        std::uint64_t x = id;
        x ^= x >> 32;
        x *= 0xd6e8feb86659fd93ULL;
        x ^= x >> 32;
        return static_cast<std::size_t>(x);
    }

    /// Knuth Algorithm R backward-shift deletion: moves only entries whose home
    /// bucket lies cyclically at or before `hole`, without re-hashing untouched keys.
    void backshift_from(std::size_t hole) {
        std::size_t i = (hole + 1) & mask_;
        while (slots_[i].id != kInvalidOrderId) {
            const std::size_t home = hash(slots_[i].id) & mask_;
            if (((i - home) & mask_) >= ((i - hole) & mask_)) {
                slots_[hole] = slots_[i];
                slots_[i] = Entry{};
                hole = i;
            }
            i = (i + 1) & mask_;
        }
    }

    std::size_t capacity_;
    std::size_t mask_;
    std::vector<Entry> slots_;
    std::size_t size_;
};

static_assert(sizeof(OrderIdIndex::Entry) == 16, "OrderIdIndex entry stays 16 bytes");

}  // namespace fastlob
