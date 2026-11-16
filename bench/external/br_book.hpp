#pragma once

// brprojects Limit-Order-Book matches a crossing limit against the other side
// before resting it, and allocates each order and each price level with new.
// In-order traversal builds a fresh vector at every AVL node.

#include "Book.hpp"
#include "Limit.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

class BrBook {
public:
    BrBook(std::size_t, std::size_t) {}

    bool add(std::uint64_t id, bool buy, std::int64_t price, std::uint32_t qty) {
        if (price <= 0 || qty == 0 || id > static_cast<std::uint64_t>(0x7fffffff)) {
            return false;
        }
        book_.addLimitOrder(static_cast<int>(id), buy, static_cast<int>(qty), static_cast<int>(price));
        return book_.searchOrderMap(static_cast<int>(id)) != nullptr;
    }

    bool cancel(std::uint64_t id) {
        if (book_.searchOrderMap(static_cast<int>(id)) == nullptr) {
            return false;
        }
        book_.cancelLimitOrder(static_cast<int>(id));
        return true;
    }

    std::uint64_t sweep_bids() {
        const std::vector<int> prices = book_.inOrderTreeTraversal(book_.getBuyTree());
        std::uint64_t sink = prices.size();
        for (int price : prices) {
            const Limit* level = book_.searchLimitMaps(price, true);
            if (level != nullptr) {
                sink += static_cast<std::uint64_t>(level->getTotalVolume());
                sink += static_cast<std::uint64_t>(price);
            }
        }
        return sink;
    }

    struct Level {
        std::int64_t price;
        std::uint64_t qty;
    };

    void collect(bool buy, std::vector<Level>& out) const {
        const std::vector<int> prices =
            book_.inOrderTreeTraversal(buy ? book_.getBuyTree() : book_.getSellTree());
        out.clear();
        out.reserve(prices.size());
        for (int price : prices) {
            const Limit* level = book_.searchLimitMaps(price, buy);
            if (level != nullptr) {
                out.push_back(Level{price, static_cast<std::uint64_t>(level->getTotalVolume())});
            }
        }
        if (buy) {
            std::reverse(out.begin(), out.end());
        }
    }

private:
    Book book_;
};
