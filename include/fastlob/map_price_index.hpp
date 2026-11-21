#pragma once

#include "fastlob/level_pool.hpp"

#include <cassert>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace fastlob {

struct AllocCounter {
    std::size_t live{0};
    std::size_t peak{0};
};

/// Counts bytes requested from the allocator. Map node overhead is whatever
/// libstdc++ asks for, not malloc's rounded usable size.
template <typename T>
struct CountingAlloc {
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    AllocCounter* counter{nullptr};

    CountingAlloc() = default;
    explicit CountingAlloc(AllocCounter* c) : counter(c) {}

    template <typename U>
    CountingAlloc(const CountingAlloc<U>& other) noexcept : counter(other.counter) {}

    [[nodiscard]] T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        if (counter != nullptr) {
            counter->live += bytes;
            if (counter->live > counter->peak) {
                counter->peak = counter->live;
            }
        }
        return static_cast<T*>(::operator new(bytes));
    }

    void deallocate(T* pointer, std::size_t n) noexcept {
        if (counter != nullptr) {
            counter->live -= n * sizeof(T);
        }
        ::operator delete(pointer);
    }

    template <typename U>
    struct rebind {
        using other = CountingAlloc<U>;
    };
};

template <typename T, typename U>
[[nodiscard]] bool operator==(const CountingAlloc<T>& lhs, const CountingAlloc<U>& rhs) noexcept {
    return lhs.counter == rhs.counter;
}

template <typename T, typename U>
[[nodiscard]] bool operator!=(const CountingAlloc<T>& lhs, const CountingAlloc<U>& rhs) noexcept {
    return !(lhs == rhs);
}

/// Ordered price index: std::map plus the shared level pool.
/// Traversal uses map iterators (amortized O(1) per step). Insert and erase are O(log n).
class MapPriceIndex {
public:
    using MapType = std::map<Price, BookLevel*, std::less<Price>,
                             CountingAlloc<std::pair<const Price, BookLevel*>>>;

    MapPriceIndex(LevelPool& pool, Side side)
        : pool_(&pool), side_(side), map_(CountingAlloc<std::pair<const Price, BookLevel*>>(&counter_)) {}

    MapPriceIndex(const MapPriceIndex&) = delete;
    MapPriceIndex& operator=(const MapPriceIndex&) = delete;

    [[nodiscard]] BookLevel* find(Price price) const {
        const auto it = map_.find(price);
        if (it == map_.end()) {
            return nullptr;
        }
        return it->second;
    }

    [[nodiscard]] BookLevel* find_or_open(Price price) {
        if (BookLevel* existing = find(price)) {
            return existing;
        }
        return open(price);
    }

    [[nodiscard]] BookLevel* open(Price price) {
        BookLevel* level = pool_->acquire(price, side_);
        if (level == nullptr) {
            return nullptr;
        }
        map_.emplace(price, level);
        if (best_ == nullptr || is_better(price, best_->price)) {
            best_ = level;
        }
        ++active_;
        return level;
    }

    void close(BookLevel* level) {
        assert(level->head == nullptr && level->tail == nullptr);
        const auto it = map_.find(level->price);
        assert(it != map_.end());
        if (best_ == level) {
            if (side_ == Side::Sell) {
                const auto next = std::next(it);
                best_ = next == map_.end() ? nullptr : next->second;
            } else if (it == map_.begin()) {
                best_ = nullptr;
            } else {
                best_ = std::prev(it)->second;
            }
        }
        map_.erase(it);
        pool_->release(level);
        assert(active_ > 0);
        --active_;
    }

    [[nodiscard]] std::size_t copy_top(std::span<LevelView> out) const {
        std::size_t n = 0;
        if (side_ == Side::Sell) {
            for (auto it = map_.begin(); it != map_.end() && n < out.size(); ++it) {
                out[n++] = LevelView{it->first, it->second->total_qty};
            }
        } else {
            for (auto it = map_.rbegin(); it != map_.rend() && n < out.size(); ++it) {
                out[n++] = LevelView{it->first, it->second->total_qty};
            }
        }
        return n;
    }

    void reserve(std::size_t /*nodes*/, std::size_t /*leaves*/) {}

    /// Bytes currently held by map nodes (released back to the allocator on erase).
    [[nodiscard]] std::size_t bytes() const { return counter_.live; }
    [[nodiscard]] std::size_t live_bytes() const { return counter_.live; }
    [[nodiscard]] std::size_t peak_bytes() const { return counter_.peak; }
    [[nodiscard]] std::size_t active() const { return active_; }
    [[nodiscard]] BookLevel* best() const { return best_; }

private:
    [[nodiscard]] bool is_better(Price candidate, Price incumbent) const {
        return side_ == Side::Buy ? candidate > incumbent : candidate < incumbent;
    }

    LevelPool* pool_;
    Side side_;
    AllocCounter counter_{};
    MapType map_;
    BookLevel* best_{nullptr};
    std::size_t active_{0};
};

}  // namespace fastlob
