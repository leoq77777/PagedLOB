#pragma once

#include "fastlob/level_pool.hpp"
#include "fastlob/price_key.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace fastlob {

/// Occupancy-bitmap helpers. Words is deduced from the array bound: 4 for an
/// 8-bit radix node, (ticks / 64) for a leaf.
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
            return (word << 6) + __builtin_ctzll(occ[word]);
        }
    }
    return -1;
}

template <int Words>
inline int occ_highest(const std::uint64_t (&occ)[Words]) {
    for (int word = Words - 1; word >= 0; --word) {
        if (occ[word] != 0) {
            return (word << 6) + 63 - __builtin_clzll(occ[word]);
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
        return (word << 6) + offset + __builtin_ctzll(shifted);
    }
    for (int next = word + 1; next < Words; ++next) {
        if (occ[next] != 0) {
            return (next << 6) + __builtin_ctzll(occ[next]);
        }
    }
    return -1;
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
        return (word << 6) + 63 - __builtin_clzll(kept);
    }
    for (int prev = word - 1; prev >= 0; --prev) {
        if (occ[prev] != 0) {
            return (prev << 6) + 63 - __builtin_clzll(occ[prev]);
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
/// array (hot); everything worse stays in the radix (cold). Each level is in
/// exactly one of the two. Empty leaves and directory nodes are recycled.
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

    /// Bitmap plus 2^PageBits 32-bit level handles.
    struct Leaf {
        std::uint64_t occ[kOccWords]{};
        std::uint32_t handle[kTicks]{};
        DirNode* parent{nullptr};
        Leaf* free_next{nullptr};
        std::uint8_t parent_digit{0};
    };

    PagedPriceIndex(LevelPool& pool, Side side) : pool_(&pool), side_(side) {
        root_ = create_node(0);
    }

    PagedPriceIndex(const PagedPriceIndex&) = delete;
    PagedPriceIndex& operator=(const PagedPriceIndex&) = delete;

    [[nodiscard]] BookLevel* find(Price price) const {
        const int off = hot_offset(price);
        if (off >= 0) {
            const std::uint32_t handle = hot_handle_[hot_index(off)];
            return handle == 0 ? nullptr : pool_->get(handle);
        }
        return radix_find(price);
    }

    [[nodiscard]] BookLevel* open(Price price) {
        BookLevel* level = pool_->acquire(price, side_);
        if (level == nullptr) {
            return nullptr;
        }
        const std::uint32_t handle = pool_->handle_of(level);
        if (best_ == nullptr) {
            clear_hot();
            hot_origin_ = price;
            hot_handle_[hot_index(0)] = handle;
            best_ = level;
        } else if (is_better(price, best_->price)) {
            rebase(price);
            hot_handle_[hot_index(0)] = handle;
            best_ = level;
        } else {
            const int off = hot_offset(price);
            if (off >= 0) {
                assert(hot_handle_[hot_index(off)] == 0);
                hot_handle_[hot_index(off)] = handle;
            } else {
                radix_put(level);
            }
        }
        ++active_;
        return level;
    }

    void close(BookLevel* level) {
        assert(level->head == nullptr && level->tail == nullptr);
        const bool was_best = best_ == level;
        const int off = hot_offset(level->price);
        if (off >= 0) {
            const int idx = hot_index(off);
            assert(hot_handle_[idx] == pool_->handle_of(level));
            hot_handle_[idx] = 0;
        } else {
            radix_remove(level);
        }
        pool_->release(level);
        assert(active_ > 0);
        --active_;
        if (was_best) {
            best_ = next_best();
            if (best_ != nullptr) {
                rebase(best_->price);
            } else {
                clear_hot();
            }
        }
    }

    [[nodiscard]] std::size_t copy_top(std::span<LevelView> out) const {
        std::size_t n = 0;
        for (int off = 0; off < kHotTicks && n < out.size(); ++off) {
            const std::uint32_t handle = hot_handle_[hot_index(off)];
            if (handle != 0) {
                out[n++] = view_of(handle);
            }
        }
        if (n >= out.size()) {
            return n;
        }
        if (side_ == Side::Sell) {
            for (Leaf* leaf = leftmost(); leaf != nullptr && n < out.size(); leaf = successor(leaf)) {
                for (int bit = occ_lowest(leaf->occ); bit >= 0 && n < out.size();
                     bit = occ_above(leaf->occ, bit)) {
                    out[n++] = view_of(leaf->handle[bit]);
                }
            }
        } else {
            for (Leaf* leaf = rightmost(); leaf != nullptr && n < out.size(); leaf = predecessor(leaf)) {
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
        assert(level->total_qty <= std::numeric_limits<Qty>::max());
        return LevelView{level->price, static_cast<Qty>(level->total_qty)};
    }

    [[nodiscard]] static std::size_t hot_bytes() {
        return sizeof(std::uint32_t) * static_cast<std::size_t>(kHotTicks);
    }

    [[nodiscard]] int hot_index(int offset) const { return (hot_base_ + offset) & kHotMask; }

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

    [[nodiscard]] std::optional<Price> price_at(Price origin, int offset) const {
        if (side_ == Side::Sell) {
            if (offset > 0 && origin > std::numeric_limits<Price>::max() - offset) {
                return std::nullopt;
            }
            return origin + offset;
        }
        if (offset > 0 && origin < std::numeric_limits<Price>::min() + offset) {
            return std::nullopt;
        }
        return origin - offset;
    }

    [[nodiscard]] int worse_delta(Price from, Price to) const {
        if (side_ == Side::Sell) {
            if (to >= from) {
                const std::uint64_t delta = static_cast<std::uint64_t>(to) - static_cast<std::uint64_t>(from);
                return delta >= static_cast<std::uint64_t>(kHotTicks) ? kHotTicks : static_cast<int>(delta);
            }
            const std::uint64_t delta = static_cast<std::uint64_t>(from) - static_cast<std::uint64_t>(to);
            return delta >= static_cast<std::uint64_t>(kHotTicks) ? -kHotTicks : -static_cast<int>(delta);
        }
        if (from >= to) {
            const std::uint64_t delta = static_cast<std::uint64_t>(from) - static_cast<std::uint64_t>(to);
            return delta >= static_cast<std::uint64_t>(kHotTicks) ? kHotTicks : static_cast<int>(delta);
        }
        const std::uint64_t delta = static_cast<std::uint64_t>(to) - static_cast<std::uint64_t>(from);
        return delta >= static_cast<std::uint64_t>(kHotTicks) ? -kHotTicks : -static_cast<int>(delta);
    }

    void clear_hot() {
        std::memset(hot_handle_, 0, sizeof(hot_handle_));
        hot_base_ = 0;
    }

    void migrate_hot_to_radix(int offset) {
        const int idx = hot_index(offset);
        const std::uint32_t handle = hot_handle_[idx];
        if (handle == 0) {
            return;
        }
        hot_handle_[idx] = 0;
        radix_put(pool_->get(handle));
    }

    void pull_radix_into_hot(int offset) {
        const auto price = price_at(hot_origin_, offset);
        if (!price.has_value()) {
            return;
        }
        BookLevel* level = radix_find(*price);
        if (level == nullptr) {
            return;
        }
        radix_remove(level);
        hot_handle_[hot_index(offset)] = pool_->handle_of(level);
    }

    void place_existing(BookLevel* level) {
        const int off = offset_from(hot_origin_, level->price);
        if (off >= 0) {
            const int idx = hot_index(off);
            assert(hot_handle_[idx] == 0);
            hot_handle_[idx] = pool_->handle_of(level);
            return;
        }
        radix_put(level);
    }

    void steal_cold_into_hot() {
        for (;;) {
            BookLevel* level = compute_best_cold();
            if (level == nullptr) {
                return;
            }
            const int off = offset_from(hot_origin_, level->price);
            if (off < 0 || off >= kHotTicks) {
                return;
            }
            radix_remove(level);
            hot_handle_[hot_index(off)] = pool_->handle_of(level);
        }
    }

    void shift_hot(int delta, Price new_origin) {
        if (delta > 0) {
            for (int i = 0; i < delta; ++i) {
                migrate_hot_to_radix(i);
            }
            hot_base_ = (hot_base_ + delta) & kHotMask;
            hot_origin_ = new_origin;
            for (int i = kHotTicks - delta; i < kHotTicks; ++i) {
                pull_radix_into_hot(i);
            }
            return;
        }
        const int d = -delta;
        for (int i = kHotTicks - d; i < kHotTicks; ++i) {
            migrate_hot_to_radix(i);
        }
        hot_base_ = (hot_base_ - d) & kHotMask;
        hot_origin_ = new_origin;
        for (int i = 0; i < d; ++i) {
            pull_radix_into_hot(i);
        }
    }

    void rebuild_hot(Price new_origin) {
        std::uint32_t saved[kHotTicks];
        std::memcpy(saved, hot_handle_, sizeof(saved));
        clear_hot();
        hot_origin_ = new_origin;
        for (int i = 0; i < kHotTicks; ++i) {
            if (saved[i] != 0) {
                place_existing(pool_->get(saved[i]));
            }
        }
        steal_cold_into_hot();
    }

    void rebase(Price new_origin) {
        if (best_ != nullptr && new_origin == hot_origin_) {
            return;
        }
        const int delta = worse_delta(hot_origin_, new_origin);
        if (best_ != nullptr && delta != 0 && delta > -kHotTicks && delta < kHotTicks) {
            shift_hot(delta, new_origin);
            return;
        }
        rebuild_hot(new_origin);
    }

    [[nodiscard]] BookLevel* next_best() const {
        for (int off = 0; off < kHotTicks; ++off) {
            const std::uint32_t handle = hot_handle_[hot_index(off)];
            if (handle != 0) {
                return pool_->get(handle);
            }
        }
        return compute_best_cold();
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
        assert(leaf != nullptr);
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
            std::memset(leaf->handle, 0, sizeof(leaf->handle));
            leaf->parent = nullptr;
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

    [[nodiscard]] Leaf* find_leaf(std::uint64_t page_key) const {
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
            leaf->parent = node;
            leaf->parent_digit = last;
            node->child[last] = leaf;
            occ_set(node->occ, last);
        }
        return static_cast<Leaf*>(node->child[last]);
    }

    void release_leaf(Leaf* leaf) {
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

    [[nodiscard]] Leaf* rightmost_from(DirNode* node, int digit) const {
        if (node->depth == kLastDirDepth) {
            return static_cast<Leaf*>(node->child[digit]);
        }
        DirNode* cursor = static_cast<DirNode*>(node->child[digit]);
        while (cursor->depth < kLastDirDepth) {
            const int bit = occ_highest(cursor->occ);
            cursor = static_cast<DirNode*>(cursor->child[bit]);
        }
        const int bit = occ_highest(cursor->occ);
        return static_cast<Leaf*>(cursor->child[bit]);
    }

    [[nodiscard]] Leaf* leftmost() const {
        if (!occ_any(root_->occ)) {
            return nullptr;
        }
        const DirNode* node = root_;
        while (node->depth < kLastDirDepth) {
            const int bit = occ_lowest(node->occ);
            node = static_cast<const DirNode*>(node->child[bit]);
        }
        const int bit = occ_lowest(node->occ);
        return static_cast<Leaf*>(node->child[bit]);
    }

    [[nodiscard]] Leaf* rightmost() const {
        if (!occ_any(root_->occ)) {
            return nullptr;
        }
        const DirNode* node = root_;
        while (node->depth < kLastDirDepth) {
            const int bit = occ_highest(node->occ);
            node = static_cast<const DirNode*>(node->child[bit]);
        }
        const int bit = occ_highest(node->occ);
        return static_cast<Leaf*>(node->child[bit]);
    }

    [[nodiscard]] Leaf* successor(Leaf* leaf) const {
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

    [[nodiscard]] Leaf* predecessor(Leaf* leaf) const {
        DirNode* node = leaf->parent;
        int digit = leaf->parent_digit;
        while (node != nullptr) {
            const int prev = occ_below(node->occ, digit);
            if (prev >= 0) {
                return rightmost_from(node, prev);
            }
            digit = node->parent_digit;
            node = node->parent;
        }
        return nullptr;
    }

    [[nodiscard]] BookLevel* compute_best_cold() const {
        if (side_ == Side::Sell) {
            Leaf* leaf = leftmost();
            if (leaf == nullptr) {
                return nullptr;
            }
            return pool_->get(leaf->handle[occ_lowest(leaf->occ)]);
        }
        Leaf* leaf = rightmost();
        if (leaf == nullptr) {
            return nullptr;
        }
        return pool_->get(leaf->handle[occ_highest(leaf->occ)]);
    }

    LevelPool* pool_;
    Side side_;
    DirNode* root_{nullptr};
    DirNode* free_nodes_{nullptr};
    Leaf* free_leaves_{nullptr};
    std::vector<std::unique_ptr<DirNode>> nodes_;
    std::vector<std::unique_ptr<Leaf>> leaves_;
    BookLevel* best_{nullptr};
    Price hot_origin_{0};
    int hot_base_{0};
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
