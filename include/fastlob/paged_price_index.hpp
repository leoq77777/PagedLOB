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
/// Words is deduced from the array bound: 4 for an 8-bit radix node, (ticks / 64) for a leaf.
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

/// Returns the sole set bit index if popcount(occ) == 1, otherwise -1.
template <int Words>
inline int occ_single_bit(const std::uint64_t (&occ)[Words]) {
    int found_word = -1;
    for (int word = 0; word < Words; ++word) {
        if (occ[word] != 0) {
            if (found_word >= 0 || (occ[word] & (occ[word] - 1)) != 0) {
                return -1;
            }
            found_word = word;
        }
    }
    if (found_word < 0) {
        return -1;
    }
    return (found_word << 6) + std::countr_zero(occ[found_word]);
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

/// Zero-migration unified paged radix price directory (`v0.5.0`):
/// - Every active `BookLevel` resides permanently in its aligned `2^PageBits`-tick `Leaf`
///   from creation until depleted, eliminating hot-cold level migration under wide spikes.
/// - `BookLevel::owner_page` enables strict O(1) `close(level)` with zero radix walk.
/// - Three-tier cache (`best_leaf_` + MRU `hot_leaf_` + `near_dir_` covering 256 adjacent
///   leaves = 65,536 ticks at default PageBits=8) resolves near-touch and wide-volatility
///   operations in O(1) without traversing upper radix levels.
/// - Tagged single-leaf path compression (`uintptr_t | 1`) avoids intermediate `DirNode`
///   chains on sparse subtrees and collapses back automatically on empty.
template <int PageBits = kDefaultPageBits, int HotTicks = 256>
class PagedPriceIndex {
public:
    static_assert(PageBits >= 5 && PageBits <= 12, "page width must fit a compact occupancy word set");
    static_assert(HotTicks >= 64 && HotTicks <= 1024 && (HotTicks & (HotTicks - 1)) == 0,
                  "hot window parameter kept for template compatibility");

    static constexpr int kPageBits = PageBits;
    static constexpr int kTicks = 1 << PageBits;
    static constexpr int kHotTicks = HotTicks;
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

    static_assert(alignof(DirNode) >= 2 && alignof(Leaf) >= 2,
                  "low pointer bit is reserved for single-leaf path compression");

    PagedPriceIndex(LevelPool& pool, Side side) : pool_(&pool), side_(side) {
        root_ = create_node(0);
        // Pre-warm 2 leaves on the free list so near-touch operations start zero-alloc out of the box.
        reserve(1, 2);
    }

    PagedPriceIndex(const PagedPriceIndex&) = delete;
    PagedPriceIndex& operator=(const PagedPriceIndex&) = delete;

    /// Pre-allocate radix directory nodes and leaves onto internal free lists.
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
        const Leaf* leaf = find_leaf(tick_page_key<PageBits>(price));
        if (leaf == nullptr) {
            return nullptr;
        }
        const std::uint32_t handle = leaf->handle[tick_page_slot<PageBits>(price)];
        return handle == 0 ? nullptr : pool_->get(handle);
    }

    /// Single-pass lookup + level creation for `StorageBook::add`.
    /// Never mutates directory state if `LevelPool` is exhausted.
    [[nodiscard]] BookLevel* find_or_open(Price price) {
        const std::uint64_t page_key = tick_page_key<PageBits>(price);
        const int slot = tick_page_slot<PageBits>(price);

        // Tier 0a: `best_leaf_` (aligned 2^PageBits neighborhood of `best_`)
        if (best_leaf_ != nullptr && best_leaf_->page_key == page_key) {
            const std::uint32_t existing = best_leaf_->handle[slot];
            if (existing != 0) {
                return pool_->get(existing);
            }
            BookLevel* level = pool_->acquire(price, side_);
            if (level == nullptr) {
                return nullptr;
            }
            level->owner_page = best_leaf_;
            occ_set(best_leaf_->occ, slot);
            best_leaf_->handle[slot] = pool_->handle_of(level);
            if (is_better(price, best_->price)) {
                best_ = level;
            }
            ++active_;
            return level;
        }

        // Tier 0b: MRU `hot_leaf_` (eliminates page-boundary straddle overhead)
        if (hot_leaf_ != nullptr && hot_leaf_->page_key == page_key) {
            const std::uint32_t existing = hot_leaf_->handle[slot];
            if (existing != 0) {
                return pool_->get(existing);
            }
            BookLevel* level = pool_->acquire(price, side_);
            if (level == nullptr) {
                return nullptr;
            }
            level->owner_page = hot_leaf_;
            occ_set(hot_leaf_->occ, slot);
            hot_leaf_->handle[slot] = pool_->handle_of(level);
            if (best_ == nullptr || is_better(price, best_->price)) {
                best_ = level;
                best_leaf_ = hot_leaf_;
            }
            ++active_;
            return level;
        }

        if (!pool_->has_free()) {
            const Leaf* leaf = find_leaf(page_key);
            if (leaf == nullptr || leaf->handle[slot] == 0) {
                return nullptr;
            }
            return pool_->get(leaf->handle[slot]);
        }

        Leaf* leaf = ensure_leaf(page_key);
        const std::uint32_t existing = leaf->handle[slot];
        if (existing != 0) {
            hot_leaf_ = leaf;
            return pool_->get(existing);
        }
        BookLevel* level = pool_->acquire(price, side_);
        assert(level != nullptr);
        level->owner_page = leaf;
        occ_set(leaf->occ, slot);
        leaf->handle[slot] = pool_->handle_of(level);
        if (best_ == nullptr || is_better(price, best_->price)) {
            if (best_leaf_ != nullptr && best_leaf_ != leaf) {
                hot_leaf_ = best_leaf_;
            }
            best_ = level;
            best_leaf_ = leaf;
            if (leaf->parent->depth == kLastDirDepth) {
                near_dir_ = leaf->parent;
                near_dir_prefix_ = page_key >> kDigitBits;
            }
        } else {
            hot_leaf_ = leaf;
        }
        ++active_;
        return level;
    }

    [[nodiscard]] BookLevel* open(Price price) {
        assert(find(price) == nullptr);
        return find_or_open(price);
    }

    void close(BookLevel* level) {
        assert(level->head == nullptr && level->tail == nullptr);
        Leaf* leaf = static_cast<Leaf*>(level->owner_page);
        assert(leaf != nullptr);
        const int slot = tick_page_slot<PageBits>(level->price);
        assert(occ_test(leaf->occ, slot) && leaf->handle[slot] == pool_->handle_of(level));
        occ_clear(leaf->occ, slot);
        leaf->handle[slot] = 0;

        if (best_ == level) {
            if (side_ == Side::Sell) {
                const int next_slot = occ_above(leaf->occ, slot);
                if (next_slot >= 0) {
                    best_ = pool_->get(leaf->handle[next_slot]);
                } else {
                    Leaf* next_leaf = leaf->next_leaf;
                    release_leaf(leaf);
                    if (next_leaf == nullptr) {
                        best_ = nullptr;
                        best_leaf_ = nullptr;
                    } else {
                        best_leaf_ = next_leaf;
                        best_ = pool_->get(next_leaf->handle[occ_lowest(next_leaf->occ)]);
                        if (next_leaf->parent->depth == kLastDirDepth) {
                            near_dir_ = next_leaf->parent;
                            near_dir_prefix_ = next_leaf->page_key >> kDigitBits;
                        }
                    }
                }
            } else {
                const int prev_slot = occ_below(leaf->occ, slot);
                if (prev_slot >= 0) {
                    best_ = pool_->get(leaf->handle[prev_slot]);
                } else {
                    Leaf* prev_leaf = leaf->prev_leaf;
                    release_leaf(leaf);
                    if (prev_leaf == nullptr) {
                        best_ = nullptr;
                        best_leaf_ = nullptr;
                    } else {
                        best_leaf_ = prev_leaf;
                        best_ = pool_->get(prev_leaf->handle[occ_highest(prev_leaf->occ)]);
                        if (prev_leaf->parent->depth == kLastDirDepth) {
                            near_dir_ = prev_leaf->parent;
                            near_dir_prefix_ = prev_leaf->page_key >> kDigitBits;
                        }
                    }
                }
            }
        } else if (!occ_any(leaf->occ)) {
            release_leaf(leaf);
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
        if (side_ == Side::Sell) {
            for (const Leaf* leaf = min_leaf_; leaf != nullptr && n < out.size();
                 leaf = leaf->next_leaf) {
                for (int w_idx = 0; w_idx < kOccWords && n < out.size(); ++w_idx) {
                    std::uint64_t w = leaf->occ[w_idx];
                    while (w != 0 && n < out.size()) {
                        const int bit = std::countr_zero(w);
                        out[n++] = view_of(leaf->handle[(w_idx << 6) | bit]);
                        w &= (w - 1);
                    }
                }
            }
        } else {
            for (const Leaf* leaf = max_leaf_; leaf != nullptr && n < out.size();
                 leaf = leaf->prev_leaf) {
                for (int w_idx = kOccWords - 1; w_idx >= 0 && n < out.size(); --w_idx) {
                    std::uint64_t w = leaf->occ[w_idx];
                    while (w != 0 && n < out.size()) {
                        const int bit = 63 - std::countl_zero(w);
                        out[n++] = view_of(leaf->handle[(w_idx << 6) | bit]);
                        w ^= (std::uint64_t{1} << bit);
                    }
                }
            }
        }
        return n;
    }

    /// Committed directory memory, including recycled nodes/leaves kept on the free list.
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
        s.live_bytes = live_nodes_ * sizeof(DirNode) + live_leaves_ * sizeof(Leaf);
        return s;
    }

    [[nodiscard]] static std::size_t dir_node_bytes() { return sizeof(DirNode); }
    [[nodiscard]] static std::size_t leaf_bytes() { return sizeof(Leaf); }

private:
    static void* tag_leaf(Leaf* leaf) {
        return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(leaf) | std::uintptr_t{1});
    }

    static bool is_tagged_leaf(const void* ptr) {
        return (reinterpret_cast<std::uintptr_t>(ptr) & std::uintptr_t{1}) != 0;
    }

    static Leaf* untag_leaf(void* ptr) {
        return reinterpret_cast<Leaf*>(reinterpret_cast<std::uintptr_t>(ptr) & ~std::uintptr_t{1});
    }

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
        if (best_leaf_ != nullptr && best_leaf_->page_key == page_key) {
            return best_leaf_;
        }
        if (hot_leaf_ != nullptr && hot_leaf_->page_key == page_key) {
            return hot_leaf_;
        }
        if (min_leaf_->page_key == page_key) {
            return min_leaf_;
        }
        if (max_leaf_->page_key == page_key) {
            return max_leaf_;
        }
        if (near_dir_ != nullptr && (page_key >> kDigitBits) == near_dir_prefix_) {
            void* p = near_dir_->child[page_key & 0xFF];
            return p == nullptr ? nullptr : untag_leaf(p);
        }
        const DirNode* node = root_;
        for (int depth = 0; depth <= kLastDirDepth; ++depth) {
            void* p = node->child[digit_of(page_key, depth)];
            if (p == nullptr) {
                return nullptr;
            }
            if (is_tagged_leaf(p)) {
                Leaf* leaf = untag_leaf(p);
                if (leaf->page_key == page_key) {
                    hot_leaf_ = leaf;
                    return leaf;
                }
                return nullptr;
            }
            node = static_cast<const DirNode*>(p);
        }
        return nullptr;
    }

    Leaf* ensure_leaf(std::uint64_t page_key) {
        if (min_leaf_ != nullptr) {
            if (best_leaf_ != nullptr && best_leaf_->page_key == page_key) {
                return best_leaf_;
            }
            if (hot_leaf_ != nullptr && hot_leaf_->page_key == page_key) {
                return hot_leaf_;
            }
            if (min_leaf_->page_key == page_key) {
                return min_leaf_;
            }
            if (max_leaf_->page_key == page_key) {
                return max_leaf_;
            }
        }
        // Tier 1: cached `depth == kLastDirDepth` directory node covering 256 adjacent pages
        if (near_dir_ != nullptr && (page_key >> kDigitBits) == near_dir_prefix_) {
            const auto last = static_cast<std::uint8_t>(page_key & 0xFF);
            void* p = near_dir_->child[last];
            if (p != nullptr) {
                return untag_leaf(p);
            }
            Leaf* leaf = create_leaf();
            leaf->page_key = page_key;
            leaf->parent = near_dir_;
            leaf->parent_digit = last;
            near_dir_->child[last] = tag_leaf(leaf);
            occ_set(near_dir_->occ, last);
            link_new_leaf(leaf);
            return leaf;
        }

        // Tier 2: path-compressed radix walk & lazy split
        DirNode* node = root_;
        for (int depth = 0; depth <= kLastDirDepth; ++depth) {
            const std::uint8_t digit = digit_of(page_key, depth);
            void* p = node->child[digit];
            if (p == nullptr) {
                Leaf* leaf = create_leaf();
                leaf->page_key = page_key;
                leaf->parent = node;
                leaf->parent_digit = digit;
                node->child[digit] = tag_leaf(leaf);
                occ_set(node->occ, digit);
                link_new_leaf(leaf);
                if (depth == kLastDirDepth) {
                    near_dir_ = node;
                    near_dir_prefix_ = page_key >> kDigitBits;
                }
                return leaf;
            }
            if (is_tagged_leaf(p)) {
                Leaf* existing = untag_leaf(p);
                if (existing->page_key == page_key) {
                    return existing;
                }
                assert(depth < kLastDirDepth);
                DirNode* sub = create_node(static_cast<std::uint8_t>(depth + 1));
                sub->parent = node;
                sub->parent_digit = digit;
                node->child[digit] = sub;
                node = sub;
                int cur_depth = depth + 1;
                while (true) {
                    const std::uint8_t d_old = digit_of(existing->page_key, cur_depth);
                    const std::uint8_t d_new = digit_of(page_key, cur_depth);
                    if (d_old != d_new) {
                        existing->parent = node;
                        existing->parent_digit = d_old;
                        node->child[d_old] = tag_leaf(existing);
                        occ_set(node->occ, d_old);

                        Leaf* leaf = create_leaf();
                        leaf->page_key = page_key;
                        leaf->parent = node;
                        leaf->parent_digit = d_new;
                        node->child[d_new] = tag_leaf(leaf);
                        occ_set(node->occ, d_new);
                        link_new_leaf(leaf);
                        if (cur_depth == kLastDirDepth) {
                            near_dir_ = node;
                            near_dir_prefix_ = page_key >> kDigitBits;
                        }
                        return leaf;
                    }
                    DirNode* next_sub = create_node(static_cast<std::uint8_t>(cur_depth + 1));
                    next_sub->parent = node;
                    next_sub->parent_digit = d_old;
                    node->child[d_old] = next_sub;
                    occ_set(node->occ, d_old);
                    node = next_sub;
                    ++cur_depth;
                }
            }
            node = static_cast<DirNode*>(p);
        }
        assert(false && "unreachable");
        return nullptr;
    }

    void release_leaf(Leaf* leaf) {
        if (leaf == best_leaf_) {
            best_leaf_ = nullptr;
        }
        if (leaf == hot_leaf_) {
            hot_leaf_ = nullptr;
        }
        unlink_leaf(leaf);
        DirNode* node = leaf->parent;
        const std::uint8_t digit = leaf->parent_digit;
        occ_clear(node->occ, digit);
        node->child[digit] = nullptr;
        recycle_leaf(leaf);

        while (node != root_ && !occ_any(node->occ)) {
            if (node == near_dir_) {
                near_dir_ = nullptr;
            }
            DirNode* parent = node->parent;
            const std::uint8_t parent_digit = node->parent_digit;
            occ_clear(parent->occ, parent_digit);
            parent->child[parent_digit] = nullptr;
            recycle_node(node);
            node = parent;
        }

        while (node != root_) {
            const int sole = occ_single_bit(node->occ);
            if (sole < 0 || !is_tagged_leaf(node->child[sole])) {
                break;
            }
            if (node == near_dir_) {
                near_dir_ = nullptr;
            }
            Leaf* survivor = untag_leaf(node->child[sole]);
            DirNode* parent = node->parent;
            const std::uint8_t parent_digit = node->parent_digit;
            parent->child[parent_digit] = tag_leaf(survivor);
            survivor->parent = parent;
            survivor->parent_digit = parent_digit;
            recycle_node(node);
            node = parent;
        }
    }

    [[nodiscard]] Leaf* leftmost_from(DirNode* node, int digit) const {
        void* p = node->child[digit];
        while (!is_tagged_leaf(p)) {
            DirNode* cursor = static_cast<DirNode*>(p);
            const int bit = occ_lowest(cursor->occ);
            assert(bit >= 0);
            p = cursor->child[bit];
        }
        return untag_leaf(p);
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

    LevelPool* pool_;
    Side side_;
    DirNode* root_{nullptr};
    DirNode* free_nodes_{nullptr};
    Leaf* free_leaves_{nullptr};
    Leaf* min_leaf_{nullptr};
    Leaf* max_leaf_{nullptr};
    Leaf* best_leaf_{nullptr};
    mutable Leaf* hot_leaf_{nullptr};
    DirNode* near_dir_{nullptr};
    std::uint64_t near_dir_prefix_{0};
    std::vector<std::unique_ptr<DirNode>> nodes_;
    std::vector<std::unique_ptr<Leaf>> leaves_;
    BookLevel* best_{nullptr};
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
