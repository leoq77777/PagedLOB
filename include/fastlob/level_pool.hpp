#pragma once

#include "fastlob/order.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fastlob {

/// Level record shared by the std::map price index and the paged directory.
/// Orders themselves stay in the baseline OrderPool; FIFO links are Order*.
/// total_qty is 64-bit so a level sum cannot wrap the way v1's uint32 total can.
struct BookLevel {
    Price price{0};
    std::uint64_t total_qty{0};
    Order* head{nullptr};
    Order* tail{nullptr};
    std::uint32_t next_free{0};
    Side side{Side::Buy};
    bool active{false};
};

inline void fifo_push(BookLevel& level, Order* order) {
    order->prev = level.tail;
    order->next = nullptr;
    if (level.tail != nullptr) {
        level.tail->next = order;
    } else {
        level.head = order;
    }
    level.tail = order;
    level.total_qty += order->qty;
}

inline void fifo_unlink(BookLevel& level, Order* order) {
    if (order->prev != nullptr) {
        order->prev->next = order->next;
    } else {
        level.head = order->next;
    }
    if (order->next != nullptr) {
        order->next->prev = order->prev;
    } else {
        level.tail = order->prev;
    }
    order->prev = nullptr;
    order->next = nullptr;
}

/// Fixed level slab. Handle 0 is null. Slots are stable for the life of the pool.
class LevelPool {
public:
    explicit LevelPool(std::size_t capacity) : storage_(capacity + 1) {
        for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(capacity); ++i) {
            storage_[i].next_free = free_head_;
            free_head_ = i;
        }
    }

    LevelPool(const LevelPool&) = delete;
    LevelPool& operator=(const LevelPool&) = delete;

    [[nodiscard]] BookLevel* acquire(Price price, Side side) {
        if (free_head_ == 0) {
            return nullptr;
        }
        const std::uint32_t handle = free_head_;
        BookLevel& level = storage_[handle];
        free_head_ = level.next_free;
        level.price = price;
        level.side = side;
        level.total_qty = 0;
        level.head = nullptr;
        level.tail = nullptr;
        level.next_free = 0;
        level.active = true;
        ++live_;
        return &level;
    }

    void release(BookLevel* level) {
        assert(level != nullptr);
        const std::uint32_t handle = handle_of(level);
        assert(handle != 0 && handle < storage_.size());
        level->active = false;
        level->head = nullptr;
        level->tail = nullptr;
        level->total_qty = 0;
        level->next_free = free_head_;
        free_head_ = handle;
        assert(live_ > 0);
        --live_;
    }

    [[nodiscard]] BookLevel* get(std::uint32_t handle) {
        assert(handle != 0 && handle < storage_.size());
        return &storage_[handle];
    }

    [[nodiscard]] const BookLevel* get(std::uint32_t handle) const {
        assert(handle != 0 && handle < storage_.size());
        return &storage_[handle];
    }

    [[nodiscard]] std::uint32_t handle_of(const BookLevel* level) const {
        assert(level != nullptr);
        const auto handle = static_cast<std::uint32_t>(level - storage_.data());
        assert(handle != 0 && handle < storage_.size());
        return handle;
    }

    [[nodiscard]] std::size_t bytes() const { return storage_.size() * sizeof(BookLevel); }
    [[nodiscard]] std::size_t live() const { return live_; }
    [[nodiscard]] std::size_t capacity() const { return storage_.empty() ? 0 : storage_.size() - 1; }

private:
    std::vector<BookLevel> storage_;
    std::uint32_t free_head_{0};
    std::size_t live_{0};
};

}  // namespace fastlob
