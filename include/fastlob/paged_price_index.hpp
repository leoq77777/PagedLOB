#pragma once

#include "fastlob/level_pool.hpp"
#include "fastlob/price_key.hpp"

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

/// Occupancy-bitmap helpers for a 256-bit page or directory node.
inline void occ_set(std::uint64_t occ[4], int bit) {
    occ[bit >> 6] |= std::uint64_t{1} << (bit & 63);
}

inline void occ_clear(std::uint64_t occ[4], int bit) {
    occ[bit >> 6] &= ~(std::uint64_t{1} << (bit & 63));
}

inline bool occ_test(const std::uint64_t occ[4], int bit) {
    return ((occ[bit >> 6] >> (bit & 63)) & 1U) != 0;
}

inline bool occ_any(const std::uint64_t occ[4]) {
    return (occ[0] | occ[1] | occ[2] | occ[3]) != 0;
}

inline int occ_lowest(const std::uint64_t occ[4]) {
    for (int word = 0; word < 4; ++word) {
        if (occ[word] != 0) {
            return (word << 6) + __builtin_ctzll(occ[word]);
        }
    }
    return -1;
}

inline int occ_highest(const std::uint64_t occ[4]) {
    for (int word = 3; word >= 0; --word) {
        if (occ[word] != 0) {
            return (word << 6) + 63 - __builtin_clzll(occ[word]);
        }
    }
    return -1;
}

/// Smallest set bit strictly greater than `bit`, or -1.
inline int occ_above(const std::uint64_t occ[4], int bit) {
    if (bit >= 255) {
        return -1;
    }
    const int start = bit + 1;
    const int word = start >> 6;
    const int offset = start & 63;
    const std::uint64_t shifted = occ[word] >> offset;
    if (shifted != 0) {
        return (word << 6) + offset + __builtin_ctzll(shifted);
    }
    for (int next = word + 1; next < 4; ++next) {
        if (occ[next] != 0) {
            return (next << 6) + __builtin_ctzll(occ[next]);
        }
    }
    return -1;
}

/// Largest set bit strictly less than `bit`, or -1.
inline int occ_below(const std::uint64_t occ[4], int bit) {
    if (bit <= 0) {
        return -1;
    }
    const int start = bit - 1;
    const int word = start >> 6;
    const int offset = start & 63;
    const std::uint64_t mask = (offset == 63) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (offset + 1)) - 1);
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

/// Eight-bit radix over the 56-bit page key (seven steps) plus a 256-tick leaf.
/// Internal nodes are full 256-wide pointer tables with an occupancy bitmap.
/// Empty leaves and the directory nodes that become empty are recycled onto a free list.
class PagedPriceIndex {
public:
    struct DirNode {
        std::uint64_t occ[4]{};
        void* child[256]{};
        DirNode* parent{nullptr};
        DirNode* free_next{nullptr};
        std::uint8_t parent_digit{0};
        std::uint8_t depth{0};
    };

    /// Bitmap plus 256 32-bit level handles. 1056 bytes before the back-pointers.
    struct Leaf {
        std::uint64_t occ[4]{};
        std::uint32_t handle[256]{};
        DirNode* parent{nullptr};
        Leaf* free_next{nullptr};
        std::uint8_t parent_digit{0};
    };

    static_assert(sizeof(std::uint64_t[4]) + sizeof(std::uint32_t[256]) == 1056,
                  "leaf payload is the proposal's ~1.03 KiB");

    PagedPriceIndex(LevelPool& pool, Side side) : pool_(&pool), side_(side) {
        root_ = create_node(0);
    }

    PagedPriceIndex(const PagedPriceIndex&) = delete;
    PagedPriceIndex& operator=(const PagedPriceIndex&) = delete;

    [[nodiscard]] BookLevel* find(Price price) const {
        const Leaf* leaf = find_leaf(tick_page_key(price));
        if (leaf == nullptr) {
            return nullptr;
        }
        const int slot = tick_page_slot(price);
        if (!occ_test(leaf->occ, slot)) {
            return nullptr;
        }
        return pool_->get(leaf->handle[slot]);
    }

    [[nodiscard]] BookLevel* open(Price price) {
        BookLevel* level = pool_->acquire(price, side_);
        if (level == nullptr) {
            return nullptr;
        }
        Leaf* leaf = ensure_leaf(tick_page_key(price));
        const int slot = tick_page_slot(price);
        assert(!occ_test(leaf->occ, slot));
        occ_set(leaf->occ, slot);
        leaf->handle[slot] = pool_->handle_of(level);
        if (best_ == nullptr || is_better(price, best_->price)) {
            best_ = level;
        }
        ++active_;
        return level;
    }

    void close(BookLevel* level) {
        assert(level->head == nullptr && level->tail == nullptr);
        const bool was_best = best_ == level;
        Leaf* leaf = find_leaf(tick_page_key(level->price));
        assert(leaf != nullptr);
        const int slot = tick_page_slot(level->price);
        assert(occ_test(leaf->occ, slot));
        occ_clear(leaf->occ, slot);
        leaf->handle[slot] = 0;
        if (!occ_any(leaf->occ)) {
            release_leaf(leaf);
        }
        pool_->release(level);
        assert(active_ > 0);
        --active_;
        if (was_best) {
            best_ = compute_best();
        }
    }

    [[nodiscard]] std::size_t copy_top(std::span<LevelView> out) const {
        std::size_t n = 0;
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
    [[nodiscard]] PageStats stats() const {
        PageStats s;
        s.committed_nodes = created_nodes_;
        s.live_nodes = live_nodes_;
        s.committed_leaves = created_leaves_;
        s.live_leaves = live_leaves_;
        s.committed_bytes = committed_bytes();
        s.live_bytes = live_nodes_ * sizeof(DirNode) + live_leaves_ * sizeof(Leaf);
        return s;
    }

    [[nodiscard]] static std::size_t dir_node_bytes() { return sizeof(DirNode); }
    [[nodiscard]] static std::size_t leaf_bytes() { return sizeof(Leaf); }

private:
    static std::uint8_t digit_of(std::uint64_t page_key, int depth) {
        return static_cast<std::uint8_t>((page_key >> ((6 - depth) * 8)) & 0xFF);
    }

    [[nodiscard]] bool is_better(Price candidate, Price incumbent) const {
        return side_ == Side::Buy ? candidate > incumbent : candidate < incumbent;
    }

    [[nodiscard]] LevelView view_of(std::uint32_t handle) const {
        const BookLevel* level = pool_->get(handle);
        assert(level->total_qty <= std::numeric_limits<Qty>::max());
        return LevelView{level->price, static_cast<Qty>(level->total_qty)};
    }

    [[nodiscard]] std::size_t committed_bytes() const {
        return created_nodes_ * sizeof(DirNode) + created_leaves_ * sizeof(Leaf);
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
        for (int depth = 0; depth < 6; ++depth) {
            const std::uint8_t digit = digit_of(page_key, depth);
            if (!occ_test(node->occ, digit)) {
                return nullptr;
            }
            node = static_cast<const DirNode*>(node->child[digit]);
        }
        const std::uint8_t last = digit_of(page_key, 6);
        if (!occ_test(node->occ, last)) {
            return nullptr;
        }
        return static_cast<Leaf*>(node->child[last]);
    }

    Leaf* ensure_leaf(std::uint64_t page_key) {
        DirNode* node = root_;
        for (int depth = 0; depth < 6; ++depth) {
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
        const std::uint8_t last = digit_of(page_key, 6);
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
        if (node->depth == 6) {
            return static_cast<Leaf*>(node->child[digit]);
        }
        DirNode* cursor = static_cast<DirNode*>(node->child[digit]);
        while (cursor->depth < 6) {
            const int bit = occ_lowest(cursor->occ);
            cursor = static_cast<DirNode*>(cursor->child[bit]);
        }
        const int bit = occ_lowest(cursor->occ);
        return static_cast<Leaf*>(cursor->child[bit]);
    }

    [[nodiscard]] Leaf* rightmost_from(DirNode* node, int digit) const {
        if (node->depth == 6) {
            return static_cast<Leaf*>(node->child[digit]);
        }
        DirNode* cursor = static_cast<DirNode*>(node->child[digit]);
        while (cursor->depth < 6) {
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
        while (node->depth < 6) {
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
        while (node->depth < 6) {
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

    [[nodiscard]] BookLevel* compute_best() const {
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
    std::size_t active_{0};
    std::size_t created_nodes_{0};
    std::size_t live_nodes_{0};
    std::size_t created_leaves_{0};
    std::size_t live_leaves_{0};
};

}  // namespace fastlob
