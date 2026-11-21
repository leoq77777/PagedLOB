#pragma once

#include "fastlob/level_pool.hpp"
#include "fastlob/price_key.hpp"

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace fastlob {

/// Occupancy-bitmap helpers using portable C++20 `<bit>` intrinsics.
/// Words is deduced from the array bound: 4 for an 8-bit radix node, (ticks / 64) for a leaf/ring.
template <int Words>
inline void occ_set(std::uint64_t (&occ)[Words], int bit) {
    occ[bit >> 6] |= std::uint64_t{1} << (bit & 63);
}

template <int Words>
inline void occ_clear(std::uint64_t (&occ)[Words], int bit) {
    occ[bit >> 6] &= ~(std::uint64_t{1} << (bit & 63));
}

template <int Words>
inline bool occ_test(const std::uint64_t (&occ)[Words], int bit) {
    return ((occ[bit >> 6] >> (bit & 63)) & 1U) != 0;
}

template <int Words>
inline bool occ_any(const std::uint64_t (&occ)[Words]) {
    std::uint64_t u = 0;
    for (int word = 0; word < Words; ++word) {
        u |= occ[word];
    }
    return u != 0;
}

template <int Words>
inline int occ_lowest(const std::uint64_t (&occ)[Words]) {
    for (int word = 0; word < Words; ++word) {
        if (occ[word] != 0) {
            return (word << 6) + std::countr_zero(occ[word]);
        }
    }
    return -1;
}

template <int Words>
inline int occ_highest(const std::uint64_t (&occ)[Words]) {
    for (int word = Words - 1; word >= 0; --word) {
        if (occ[word] != 0) {
            return (word << 6) + 63 - std::countl_zero(occ[word]);
        }
    }
    return -1;
}

/// Smallest set bit strictly greater than `bit`, or -1.
template <int Words>
inline int occ_above(const std::uint64_t (&occ)[Words], int bit) {
    const int last = Words * 64 - 1;
    if (bit >= last) {
        return -1;
    }
    const int start = bit + 1;
    const int word = start >> 6;
    const int offset = start & 63;
    const std::uint64_t shifted = occ[word] >> offset;
    if (shifted != 0) {
        return (word << 6) + offset + std::countr_zero(shifted);
    }
    for (int next = word + 1; next < Words; ++next) {
        if (occ[next] != 0) {
            return (next << 6) + std::countr_zero(occ[next]);
        }
    }
    return -1;
}

/// Smallest set bit greater than or equal to `bit`, or -1.
template <int Words>
inline int occ_at_or_above(const std::uint64_t (&occ)[Words], int bit) {
    return bit <= 0 ? occ_lowest(occ) : occ_above(occ, bit - 1);
}

/// Largest set bit strictly less than `bit`, or -1.
template <int Words>
inline int occ_below(const std::uint64_t (&occ)[Words], int bit) {
    if (bit <= 0) {
        return -1;
    }
    const int start = bit - 1;
    const int word = start >> 6;
    const int offset = start & 63;
    const std::uint64_t mask =
        (offset == 63) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (offset + 1)) - 1);
    const std::uint64_t kept = occ[word] & mask;
    if (kept != 0) {
        return (word << 6) + 63 - std::countl_zero(kept);
    }
    for (int prev = word - 1; prev >= 0; --prev) {
        if (occ[prev] != 0) {
            return (prev << 6) + 63 - std::countl_zero(occ[prev]);
        }
    }
    return -1;
}

struct PageStats {
    std::size_t committed_nodes{0};
    std::size_t live_nodes{0};
    std::size_t committed_leaves{0};
    std::size_t live_leaves{0};
    std::size_t committed_bytes{0};
    std::size_t live_bytes{0};

    PageStats& operator+=(const PageStats& other) {
        committed_nodes += other.committed_nodes;
        live_nodes += other.live_nodes;
        committed_leaves += other.committed_leaves;
        live_leaves += other.live_leaves;
        committed_bytes += other.committed_bytes;
        live_bytes += other.live_bytes;
        return *this;
    }
};

inline PageStats operator+(PageStats lhs, const PageStats& rhs) {
    lhs += rhs;
    return lhs;
}

/// Eight-bit radix over the high (64 - PageBits) of the encoded tick, plus a
/// 2^PageBits-tick leaf. The nearest HotTicks ticks from best live in a dense
/// ring with an occupancy bitmap (hot); everything worse stays in the radix
/// (cold) with an intrusive sorted leaf list. Each level sits in exactly one tier.
template <int PageBits = kDefaultPageBits, int HotTicks = 256>
class PagedPriceIndex {
public:
    static_assert(PageBits >= 5 && PageBits <= 12, "page width must fit a compact occupancy word set");
    static_assert(HotTicks >= 64 && HotTicks <= 1024 && (HotTicks & (HotTicks - 1)) == 0,
                  "hot window is a power-of-two ring");

    static constexpr int kPageBits = PageBits;
    static constexpr int kTicks = 1 << PageBits;
    static constexpr int kHotTicks = HotTicks;
    static constexpr int kHotMask = HotTicks - 1;
    static constexpr int kHotOccWords = HotTicks / 64;
    static constexpr int kOccWords = (kTicks + 63) / 64;
    static constexpr int kKeyBits = 64 - PageBits;
    static constexpr int kDigitBits = 8;
    static constexpr int kDigits = (kKeyBits + kDigitBits - 1) / kDigitBits;
    static constexpr int kLastDirDepth = kDigits - 1;

    struct DirNode {
        std::uint64_t occ[4]{};
        void* child[256]{};
        DirNode* parent{nullptr};
        DirNode* free_next{nullptr};
        std::uint8_t parent_digit{0};
        std::uint8_t depth{0};
    };

    /// Header + bitmap share the first cache line; 32-bit level handles follow.
    struct Leaf {
        std::uint64_t occ[kOccWords]{};
        std::uint64_t page_key{0};
        Leaf* prev_leaf{nullptr};
        Leaf* next_leaf{nullptr};
        DirNode* parent{nullptr};
        Leaf* free_next{nullptr};
        std::uint8_t parent_digit{0};
        std::uint32_t handle[kTicks]{};
    };

    PagedPriceIndex(LevelPool& pool, Side side) : pool_(&pool), side_(side) {
        root_ = create_node(0);
    }

    PagedPriceIndex(const PagedPriceIndex&) = delete;
    PagedPriceIndex& operator=(const PagedPriceIndex&) = delete;

    /// Pre-allocate cold radix directory nodes and leaves onto internal free lists.
    void reserve(std::size_t nodes, std::size_t leaves) {
        while (created_nodes_ < nodes) {
            auto owned = std::make_unique<DirNode>();
            DirNode* raw = owned.get();
            nodes_.push_back(std::move(owned));
            ++created_nodes_;
            raw->free_next = free_nodes_;
            free_nodes_ = raw;
        }
        while (created_leaves_ < leaves) {
            auto owned = std::make_unique<Leaf>();
            Leaf* raw = owned.get();
            leaves_.push_back(std::move(owned));
            ++created_leaves_;
            raw->free_next = free_leaves_;
            free_leaves_ = raw;
        }
    }

    [[nodiscard]] BookLevel* find(Price price) const {
        if (best_ == nullptr) {
            return nullptr;
        }
        const int off = offset_from(hot_origin_, price);
        if (off >= 0) {
            const std::uint32_t handle = hot_handle_[hot_index(off)];
            return handle == 0 ? nullptr : pool_->get(handle);
        }
        return live_leaves_ == 0 ? nullptr : radix_find(price);
    }

    /// Single-pass lookup + level creation for `StorageBook::add`.
    /// Never mutates directory state if `LevelPool` is exhausted.
    [[nodiscard]] BookLevel* find_or_open(Price price) {
        if (best_ == nullptr) {
            BookLevel* level = pool_->acquire(price, side_);
            if (level == nullptr) {
                return nullptr;
            }
            assert(hot_count_ == 0);
            hot_base_ = 0;
            hot_origin_ = price;
            hot_put_at_idx(0, pool_->handle_of(level));
            best_ = level;
            ++active_;
            return level;
        }
        if (is_better(price, best_->price)) {
            BookLevel* level = pool_->acquire(price, side_);
            if (level == nullptr) {
                return nullptr;
            }
            promote_better_origin(price);
            hot_put_at_idx(hot_index(0), pool_->handle_of(level));
            best_ = level;
            ++active_;
            return level;
        }
        const int off = offset_from(hot_origin_, price);
        if (off >= 0) {
            const int idx = hot_index(off);
            const std::uint32_t existing = hot_handle_[idx];
            if (existing != 0) {
                return pool_->get(existing);
            }
            BookLevel* level = pool_->acquire(price, side_);
            if (level == nullptr) {
                return nullptr;
            }
            hot_put_at_idx(idx, pool_->handle_of(level));
            ++active_;
            return level;
        }
        const std::uint64_t page_key = tick_page_key<PageBits>(price);
        const int slot = tick_page_slot<PageBits>(price);
        if (!pool_->has_free()) {
            if (live_leaves_ == 0) {
                return nullptr;
            }
            const Leaf* leaf = find_leaf(page_key);
            if (leaf == nullptr || !occ_test(leaf->occ, slot)) {
                return nullptr;
            }
            return pool_->get(leaf->handle[slot]);
        }
        Leaf* leaf = ensure_leaf(page_key);
        if (occ_test(leaf->occ, slot)) {
            return pool_->get(leaf->handle[slot]);
        }
        BookLevel* level = pool_->acquire(price, side_);
        assert(level != nullptr);
        occ_set(leaf->occ, slot);
        leaf->handle[slot] = pool_->handle_of(level);
        ++active_;
        return level;
    }

    [[nodiscard]] BookLevel* open(Price price) {
        assert(find(price) == nullptr);
        return find_or_open(price);
    }

    void close(BookLevel* level) {
        assert(level->head == nullptr && level->tail == nullptr);
        if (best_ == level) {
            const std::uint32_t removed = hot_take_at_idx(hot_index(0));
            assert(removed == pool_->handle_of(level));
            (void)removed;
            pool_->release(level);
            assert(active_ > 0);
            --active_;
            if (hot_count_ > 0) {
                int next_idx = occ_at_or_above(hot_occ_, hot_base_);
                if (next_idx < 0) {
                    next_idx = occ_lowest(hot_occ_);
                }
                assert(next_idx >= 0);
                BookLevel* next = pool_->get(hot_handle_[next_idx]);
                hot_base_ = next_idx;
                hot_origin_ = next->price;
                best_ = next;
                if (live_leaves_ > 0) {
                    steal_cold_into_hot();
                }
            } else {
                hot_base_ = 0;
                if (live_leaves_ == 0) {
                    best_ = nullptr;
                } else {
                    BookLevel* cold = compute_best_cold();
                    assert(cold != nullptr);
                    hot_origin_ = cold->price;
                    best_ = cold;
                    steal_cold_into_hot();
                }
            }
            return;
        }
        const int off = hot_offset(level->price);
        if (off >= 0) {
            const std::uint32_t removed = hot_take_at_idx(hot_index(off));
            assert(removed == pool_->handle_of(level));
            (void)removed;
        } else {
            radix_remove(level);
        }
        pool_->release(level);
        assert(active_ > 0);
        --active_;
    }

    [[nodiscard]] std::size_t copy_top(std::span<LevelView> out) const {
        if (best_ == nullptr || out.empty()) {
            return 0;
        }
        std::size_t n = 0;
        if (hot_count_ > 0) {
            const int start_word = hot_base_ >> 6;
            for (int w_idx = start_word; w_idx < kHotOccWords && n < out.size(); ++w_idx) {
                std::uint64_t w = hot_occ_[w_idx];
                if (w_idx == start_word) {
                    w &= (~0ULL << (hot_base_ & 63));
                }
                while (w != 0 && n < out.size()) {
                    const int bit = std::countr_zero(w);
                    out[n++] = view_of(hot_handle_[(w_idx << 6) | bit]);
                    w &= (w - 1);
                }
            }
            if (hot_base_ > 0 && n < out.size()) {
                const int stop_word = hot_base_ >> 6;
                for (int w_idx = 0; w_idx < stop_word && n < out.size(); ++w_idx) {
                    std::uint64_t w = hot_occ_[w_idx];
                    while (w != 0 && n < out.size()) {
                        const int bit = std::countr_zero(w);
                        out[n++] = view_of(hot_handle_[(w_idx << 6) | bit]);
                        w &= (w - 1);
                    }
                }
                const int rem_bits = hot_base_ & 63;
                if (rem_bits != 0 && n < out.size()) {
                    std::uint64_t w = hot_occ_[stop_word] & ((1ULL << rem_bits) - 1);
                    while (w != 0 && n < out.size()) {
                        const int bit = std::countr_zero(w);
                        out[n++] = view_of(hot_handle_[(stop_word << 6) | bit]);
                        w &= (w - 1);
                    }
                }
            }
        }
        if (n >= out.size() || live_leaves_ == 0) {
            return n;
        }
        if (side_ == Side::Sell) {
            for (const Leaf* leaf = min_leaf_; leaf != nullptr && n < out.size();
                 leaf = leaf->next_leaf) {
                for (int bit = occ_lowest(leaf->occ); bit >= 0 && n < out.size();
                     bit = occ_above(leaf->occ, bit)) {
                    out[n++] = view_of(leaf->handle[bit]);
                }
            }
        } else {
            for (const Leaf* leaf = max_leaf_; leaf != nullptr && n < out.size();
                 leaf = leaf->prev_leaf) {
                for (int bit = occ_highest(leaf->occ); bit >= 0 && n < out.size();
                     bit = occ_below(leaf->occ, bit)) {
                    out[n++] = view_of(leaf->handle[bit]);
                }
            }
        }
        return n;
    }

    /// Committed directory memory, including recycled nodes kept on the free list.
    [[nodiscard]] std::size_t bytes() const { return committed_bytes(); }
    [[nodiscard]] std::size_t live_bytes() const { return stats().live_bytes; }
    [[nodiscard]] std::size_t peak_bytes() const { return committed_bytes(); }
    [[nodiscard]] std::size_t active() const { return active_; }
    [[nodiscard]] BookLevel* best() const { return best_; }
    [[nodiscard]] PageStats stats() const {
        PageStats s;
        s.committed_nodes = created_nodes_;
        s.live_nodes = live_nodes_;
        s.committed_leaves = created_leaves_;
        s.live_leaves = live_leaves_;
        s.committed_bytes = committed_bytes();
        s.live_bytes = live_nodes_ * sizeof(DirNode) + live_leaves_ * sizeof(Leaf) + hot_bytes();
        return s;
    }

    [[nodiscard]] static std::size_t dir_node_bytes() { return sizeof(DirNode); }
    [[nodiscard]] static std::size_t leaf_bytes() { return sizeof(Leaf); }

private:
    static std::uint8_t digit_of(std::uint64_t page_key, int depth) {
        return static_cast<std::uint8_t>((page_key >> ((kLastDirDepth - depth) * kDigitBits)) & 0xFF);
    }

    [[nodiscard]] bool is_better(Price candidate, Price incumbent) const {
        return side_ == Side::Buy ? candidate > incumbent : candidate < incumbent;
    }

    [[nodiscard]] LevelView view_of(std::uint32_t handle) const {
        const BookLevel* level = pool_->get(handle);
        return LevelView{level->price, level->total_qty};
    }

    [[nodiscard]] static std::size_t hot_bytes() {
        return sizeof(std::uint32_t) * static_cast<std::size_t>(kHotTicks) +
               sizeof(std::uint64_t) * static_cast<std::size_t>(kHotOccWords);
    }

    [[nodiscard]] int hot_index(int offset) const { return (hot_base_ + offset) & kHotMask; }

    void hot_put_at_idx(int idx, std::uint32_t handle) {
        assert(handle != 0 && hot_handle_[idx] == 0 && !occ_test(hot_occ_, idx));
        hot_handle_[idx] = handle;
        occ_set(hot_occ_, idx);
        ++hot_count_;
    }

    std::uint32_t hot_take_at_idx(int idx) {
        const std::uint32_t handle = hot_handle_[idx];
        if (handle != 0) {
            hot_handle_[idx] = 0;
            occ_clear(hot_occ_, idx);
            assert(hot_count_ > 0);
            --hot_count_;
        }
        return handle;
    }

    [[nodiscard]] std::size_t committed_bytes() const {
        return created_nodes_ * sizeof(DirNode) + created_leaves_ * sizeof(Leaf) + hot_bytes();
    }

    [[nodiscard]] int hot_offset(Price price) const {
        if (best_ == nullptr) {
            return -1;
        }
        return offset_from(hot_origin_, price);
    }

    [[nodiscard]] int offset_from(Price origin, Price price) const {
        if (side_ == Side::Sell) {
            if (price < origin) {
                return -1;
            }
            const std::uint64_t delta = static_cast<std::uint64_t>(price) - static_cast<std::uint64_t>(origin);
            if (delta >= static_cast<std::uint64_t>(kHotTicks)) {
                return -1;
            }
            return static_cast<int>(delta);
        }
        if (price > origin) {
            return -1;
        }
        const std::uint64_t delta = static_cast<std::uint64_t>(origin) - static_cast<std::uint64_t>(price);
        if (delta >= static_cast<std::uint64_t>(kHotTicks)) {
            return -1;
        }
        return static_cast<int>(delta);
    }

    [[nodiscard]] int improvement_delta(Price old_origin, Price better_origin) const {
        const std::uint64_t delta = side_ == Side::Sell
                                        ? static_cast<std::uint64_t>(old_origin) -
                                              static_cast<std::uint64_t>(better_origin)
                                        : static_cast<std::uint64_t>(better_origin) -
                                              static_cast<std::uint64_t>(old_origin);
        return delta >= static_cast<std::uint64_t>(kHotTicks) ? kHotTicks : static_cast<int>(delta);
    }

    /// Shift or reset the hot window when a strictly better price arrives.
    /// Cold levels are all worse than `old_origin + kHotTicks`, so none can enter `[new_origin, +kHotTicks)`.
    void promote_better_origin(Price new_origin) {
        const int d = improvement_delta(hot_origin_, new_origin);
        assert(d >= 1 && d <= kHotTicks);
        if (d == 1) {
            const std::uint32_t h = hot_take_at_idx(hot_index(kHotTicks - 1));
            if (h != 0) {
                radix_put(pool_->get(h));
            }
            hot_base_ = (hot_base_ - 1) & kHotMask;
            hot_origin_ = new_origin;
            return;
        }
        if (static_cast<std::size_t>(d) < hot_count_) {
            for (int off = kHotTicks - d; off < kHotTicks; ++off) {
                const std::uint32_t h = hot_take_at_idx(hot_index(off));
                if (h != 0) {
                    radix_put(pool_->get(h));
                }
            }
        } else {
            for (int idx = occ_lowest(hot_occ_); idx >= 0; idx = occ_above(hot_occ_, idx)) {
                const int off = (idx - hot_base_) & kHotMask;
                if (off >= kHotTicks - d) {
                    const std::uint32_t h = hot_take_at_idx(idx);
                    radix_put(pool_->get(h));
                }
            }
        }
        hot_base_ = (hot_count_ == 0) ? 0 : ((hot_base_ - d) & kHotMask);
        hot_origin_ = new_origin;
    }

    /// Pull cold levels that now fall within `< kHotTicks` of `hot_origin_` using the sorted leaf list.
    void steal_cold_into_hot() {
        if (side_ == Side::Sell) {
            while (min_leaf_ != nullptr) {
                const int slot = occ_lowest(min_leaf_->occ);
                assert(slot >= 0);
                const std::uint32_t handle = min_leaf_->handle[slot];
                BookLevel* level = pool_->get(handle);
                const int off = offset_from(hot_origin_, level->price);
                if (off < 0) {
                    return;
                }
                occ_clear(min_leaf_->occ, slot);
                min_leaf_->handle[slot] = 0;
                if (!occ_any(min_leaf_->occ)) {
                    release_leaf(min_leaf_);
                }
                hot_put_at_idx(hot_index(off), handle);
            }
            return;
        }
        while (max_leaf_ != nullptr) {
            const int slot = occ_highest(max_leaf_->occ);
            assert(slot >= 0);
            const std::uint32_t handle = max_leaf_->handle[slot];
            BookLevel* level = pool_->get(handle);
            const int off = offset_from(hot_origin_, level->price);
            if (off < 0) {
                return;
            }
            occ_clear(max_leaf_->occ, slot);
            max_leaf_->handle[slot] = 0;
            if (!occ_any(max_leaf_->occ)) {
                release_leaf(max_leaf_);
            }
            hot_put_at_idx(hot_index(off), handle);
        }
    }

    [[nodiscard]] BookLevel* radix_find(Price price) const {
        const Leaf* leaf = find_leaf(tick_page_key<PageBits>(price));
        if (leaf == nullptr) {
            return nullptr;
        }
        const int slot = tick_page_slot<PageBits>(price);
        if (!occ_test(leaf->occ, slot)) {
            return nullptr;
        }
        return pool_->get(leaf->handle[slot]);
    }

    void radix_put(BookLevel* level) {
        Leaf* leaf = ensure_leaf(tick_page_key<PageBits>(level->price));
        const int slot = tick_page_slot<PageBits>(level->price);
        assert(!occ_test(leaf->occ, slot));
        occ_set(leaf->occ, slot);
        leaf->handle[slot] = pool_->handle_of(level);
    }

    void radix_remove(BookLevel* level) {
        Leaf* leaf = find_leaf(tick_page_key<PageBits>(level->price));
        if (leaf == nullptr) {
            assert(false && "cold leaf missing on radix_remove");
            return;
        }
        const int slot = tick_page_slot<PageBits>(level->price);
        assert(occ_test(leaf->occ, slot));
        occ_clear(leaf->occ, slot);
        leaf->handle[slot] = 0;
        if (!occ_any(leaf->occ)) {
            release_leaf(leaf);
        }
    }

    DirNode* create_node(std::uint8_t depth) {
        DirNode* node = nullptr;
        if (free_nodes_ != nullptr) {
            node = free_nodes_;
            free_nodes_ = node->free_next;
            std::memset(node->occ, 0, sizeof(node->occ));
            std::memset(node->child, 0, sizeof(node->child));
            node->parent = nullptr;
            node->free_next = nullptr;
            node->parent_digit = 0;
        } else {
            auto owned = std::make_unique<DirNode>();
            node = owned.get();
            nodes_.push_back(std::move(owned));
            ++created_nodes_;
        }
        node->depth = depth;
        ++live_nodes_;
        return node;
    }

    Leaf* create_leaf() {
        Leaf* leaf = nullptr;
        if (free_leaves_ != nullptr) {
            leaf = free_leaves_;
            free_leaves_ = leaf->free_next;
            std::memset(leaf->occ, 0, sizeof(leaf->occ));
            leaf->page_key = 0;
            leaf->parent = nullptr;
            leaf->prev_leaf = nullptr;
            leaf->next_leaf = nullptr;
            leaf->free_next = nullptr;
            leaf->parent_digit = 0;
        } else {
            auto owned = std::make_unique<Leaf>();
            leaf = owned.get();
            leaves_.push_back(std::move(owned));
            ++created_leaves_;
        }
        ++live_leaves_;
        return leaf;
    }

    void recycle_node(DirNode* node) {
        node->free_next = free_nodes_;
        free_nodes_ = node;
        assert(live_nodes_ > 0);
        --live_nodes_;
    }

    void recycle_leaf(Leaf* leaf) {
        leaf->free_next = free_leaves_;
        free_leaves_ = leaf;
        assert(live_leaves_ > 0);
        --live_leaves_;
    }

    void link_new_leaf(Leaf* leaf) {
        if (min_leaf_ == nullptr) {
            min_leaf_ = leaf;
            max_leaf_ = leaf;
            leaf->prev_leaf = nullptr;
            leaf->next_leaf = nullptr;
            return;
        }
        if (leaf->page_key > max_leaf_->page_key) {
            leaf->prev_leaf = max_leaf_;
            leaf->next_leaf = nullptr;
            max_leaf_->next_leaf = leaf;
            max_leaf_ = leaf;
            return;
        }
        if (leaf->page_key < min_leaf_->page_key) {
            leaf->prev_leaf = nullptr;
            leaf->next_leaf = min_leaf_;
            min_leaf_->prev_leaf = leaf;
            min_leaf_ = leaf;
            return;
        }
        Leaf* next = tree_successor(leaf);
        assert(next != nullptr && next->prev_leaf != nullptr);
        Leaf* prev = next->prev_leaf;
        leaf->prev_leaf = prev;
        leaf->next_leaf = next;
        prev->next_leaf = leaf;
        next->prev_leaf = leaf;
    }

    void unlink_leaf(Leaf* leaf) {
        if (leaf->prev_leaf != nullptr) {
            leaf->prev_leaf->next_leaf = leaf->next_leaf;
        } else {
            min_leaf_ = leaf->next_leaf;
        }
        if (leaf->next_leaf != nullptr) {
            leaf->next_leaf->prev_leaf = leaf->prev_leaf;
        } else {
            max_leaf_ = leaf->prev_leaf;
        }
        leaf->prev_leaf = nullptr;
        leaf->next_leaf = nullptr;
    }

    [[nodiscard]] Leaf* find_leaf(std::uint64_t page_key) const {
        if (min_leaf_ == nullptr) {
            return nullptr;
        }
        if (min_leaf_->page_key == page_key) {
            return min_leaf_;
        }
        if (max_leaf_->page_key == page_key) {
            return max_leaf_;
        }
        const DirNode* node = root_;
        for (int depth = 0; depth < kLastDirDepth; ++depth) {
            const std::uint8_t digit = digit_of(page_key, depth);
            if (!occ_test(node->occ, digit)) {
                return nullptr;
            }
            node = static_cast<const DirNode*>(node->child[digit]);
        }
        const std::uint8_t last = digit_of(page_key, kLastDirDepth);
        if (!occ_test(node->occ, last)) {
            return nullptr;
        }
        return static_cast<Leaf*>(node->child[last]);
    }

    Leaf* ensure_leaf(std::uint64_t page_key) {
        if (min_leaf_ != nullptr) {
            if (min_leaf_->page_key == page_key) {
                return min_leaf_;
            }
            if (max_leaf_->page_key == page_key) {
                return max_leaf_;
            }
        }
        DirNode* node = root_;
        for (int depth = 0; depth < kLastDirDepth; ++depth) {
            const std::uint8_t digit = digit_of(page_key, depth);
            if (node->child[digit] == nullptr) {
                DirNode* child = create_node(static_cast<std::uint8_t>(node->depth + 1));
                child->parent = node;
                child->parent_digit = digit;
                node->child[digit] = child;
                occ_set(node->occ, digit);
            }
            node = static_cast<DirNode*>(node->child[digit]);
        }
        const std::uint8_t last = digit_of(page_key, kLastDirDepth);
        if (node->child[last] == nullptr) {
            Leaf* leaf = create_leaf();
            leaf->page_key = page_key;
            leaf->parent = node;
            leaf->parent_digit = last;
            node->child[last] = leaf;
            occ_set(node->occ, last);
            link_new_leaf(leaf);
        }
        return static_cast<Leaf*>(node->child[last]);
    }

    void release_leaf(Leaf* leaf) {
        unlink_leaf(leaf);
        DirNode* node = leaf->parent;
        const std::uint8_t digit = leaf->parent_digit;
        occ_clear(node->occ, digit);
        node->child[digit] = nullptr;
        recycle_leaf(leaf);
        while (node != root_ && !occ_any(node->occ)) {
            DirNode* parent = node->parent;
            const std::uint8_t parent_digit = node->parent_digit;
            occ_clear(parent->occ, parent_digit);
            parent->child[parent_digit] = nullptr;
            recycle_node(node);
            node = parent;
        }
    }

    [[nodiscard]] Leaf* leftmost_from(DirNode* node, int digit) const {
        if (node->depth == kLastDirDepth) {
            return static_cast<Leaf*>(node->child[digit]);
        }
        DirNode* cursor = static_cast<DirNode*>(node->child[digit]);
        while (cursor->depth < kLastDirDepth) {
            const int bit = occ_lowest(cursor->occ);
            cursor = static_cast<DirNode*>(cursor->child[bit]);
        }
        const int bit = occ_lowest(cursor->occ);
        return static_cast<Leaf*>(cursor->child[bit]);
    }

    [[nodiscard]] Leaf* tree_successor(Leaf* leaf) const {
        DirNode* node = leaf->parent;
        int digit = leaf->parent_digit;
        while (node != nullptr) {
            const int next = occ_above(node->occ, digit);
            if (next >= 0) {
                return leftmost_from(node, next);
            }
            digit = node->parent_digit;
            node = node->parent;
        }
        return nullptr;
    }

    [[nodiscard]] BookLevel* compute_best_cold() const {
        if (side_ == Side::Sell) {
            if (min_leaf_ == nullptr) {
                return nullptr;
            }
            return pool_->get(min_leaf_->handle[occ_lowest(min_leaf_->occ)]);
        }
        if (max_leaf_ == nullptr) {
            return nullptr;
        }
        return pool_->get(max_leaf_->handle[occ_highest(max_leaf_->occ)]);
    }

    LevelPool* pool_;
    Side side_;
    DirNode* root_{nullptr};
    DirNode* free_nodes_{nullptr};
    Leaf* free_leaves_{nullptr};
    Leaf* min_leaf_{nullptr};
    Leaf* max_leaf_{nullptr};
    std::vector<std::unique_ptr<DirNode>> nodes_;
    std::vector<std::unique_ptr<Leaf>> leaves_;
    BookLevel* best_{nullptr};
    Price hot_origin_{0};
    int hot_base_{0};
    std::size_t hot_count_{0};
    std::uint64_t hot_occ_[kHotOccWords]{};
    std::uint32_t hot_handle_[kHotTicks]{};
    std::size_t active_{0};
    std::size_t created_nodes_{0};
    std::size_t live_nodes_{0};
    std::size_t created_leaves_{0};
    std::size_t live_leaves_{0};
};

static_assert(sizeof(std::uint64_t[4]) + sizeof(std::uint32_t[256]) == 1056,
              "256-tick leaf payload is the proposal's ~1.03 KiB");
static_assert(PagedPriceIndex<8>::kLastDirDepth == 6, "256-tick directory stays seven 8-bit steps");
static_assert(PagedPriceIndex<8>::kTicks == 256, "default page is 256 ticks");

}  // namespace fastlob
