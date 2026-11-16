#pragma once

#include "fastlob/lob_fast.hpp"
#include "fastlob/storage_book.hpp"

#include <cstdint>
#include <vector>

class FastV1Book {
public:
    FastV1Book(std::size_t orders, std::size_t levels) : book_(orders, levels), scratch_(levels + 8) {}

    bool add(std::uint64_t id, bool buy, std::int64_t price, std::uint32_t qty) {
        return book_.add(id, buy ? fastlob::Side::Buy : fastlob::Side::Sell, price, qty);
    }
    bool cancel(std::uint64_t id) { return book_.cancel(id); }

    std::uint64_t sweep_bids() {
        const auto n = book_.top_n(fastlob::Side::Buy, scratch_);
        std::uint64_t sink = n;
        for (std::size_t i = 0; i < n; ++i) {
            sink += scratch_[i].qty;
            sink += static_cast<std::uint64_t>(scratch_[i].price);
        }
        return sink;
    }

    fastlob::FastOrderBook& native() { return book_; }

    struct Level {
        std::int64_t price;
        std::uint64_t qty;
    };

    void collect(bool buy, std::vector<Level>& out) const {
        std::vector<fastlob::LevelView> levels(scratch_.size());
        const auto n = book_.top_n(buy ? fastlob::Side::Buy : fastlob::Side::Sell, levels);
        out.clear();
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(Level{levels[i].price, levels[i].qty});
        }
    }

private:
    fastlob::FastOrderBook book_;
    std::vector<fastlob::LevelView> scratch_;
};

class FastPagedBook {
public:
    FastPagedBook(std::size_t orders, std::size_t levels) : book_(orders, levels), scratch_(levels + 8) {}

    bool add(std::uint64_t id, bool buy, std::int64_t price, std::uint32_t qty) {
        return book_.add(id, buy ? fastlob::Side::Buy : fastlob::Side::Sell, price, qty);
    }
    bool cancel(std::uint64_t id) { return book_.cancel(id); }

    std::uint64_t sweep_bids() {
        const auto n = book_.top_n(fastlob::Side::Buy, scratch_);
        std::uint64_t sink = n;
        for (std::size_t i = 0; i < n; ++i) {
            sink += scratch_[i].qty;
            sink += static_cast<std::uint64_t>(scratch_[i].price);
        }
        return sink;
    }

    fastlob::PagedStorageBook& native() { return book_; }

    struct Level {
        std::int64_t price;
        std::uint64_t qty;
    };

    void collect(bool buy, std::vector<Level>& out) const {
        std::vector<fastlob::LevelView> levels(scratch_.size());
        const auto n = book_.top_n(buy ? fastlob::Side::Buy : fastlob::Side::Sell, levels);
        out.clear();
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(Level{levels[i].price, levels[i].qty});
        }
    }

private:
    fastlob::PagedStorageBook book_;
    std::vector<fastlob::LevelView> scratch_;
};
