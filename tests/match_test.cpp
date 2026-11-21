#include "fastlob/storage_book.hpp"

#include <gtest/gtest.h>

#include <vector>

using fastlob::Fill;
using fastlob::MapStorageBook;
using fastlob::PagedLOB;
using fastlob::PagedStorageBook;
using fastlob::Side;

template <typename Book>
class MatchBook : public ::testing::Test {
protected:
    Book book{1024, 1024};
};

using BookTypes = ::testing::Types<PagedStorageBook, MapStorageBook>;
TYPED_TEST_SUITE(MatchBook, BookTypes);

TYPED_TEST(MatchBook, ExactCrossClearsBoth) {
    ASSERT_TRUE(this->book.add(1, Side::Sell, 100, 10));
    std::vector<Fill> fills;
    const auto result = this->book.match_limit(2, Side::Buy, 100, 10, 0, &fills);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.filled, 10u);
    EXPECT_EQ(result.remaining, 0u);
    EXPECT_FALSE(result.rested);
    EXPECT_EQ(this->book.order_count(), 0u);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].maker_id, 1u);
    EXPECT_EQ(fills[0].taker_id, 2u);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[0].qty, 10u);
}

TYPED_TEST(MatchBook, NoCrossRests) {
    ASSERT_TRUE(this->book.add(1, Side::Sell, 101, 10));
    const auto result = this->book.match_limit(2, Side::Buy, 100, 10);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.filled, 0u);
    EXPECT_TRUE(result.rested);
    EXPECT_EQ(this->book.order_count(), 2u);
    EXPECT_EQ(this->book.best_price(Side::Buy), 100);
    EXPECT_EQ(this->book.best_price(Side::Sell), 101);
}

TYPED_TEST(MatchBook, WalksLevelsAtMakerPrice) {
    ASSERT_TRUE(this->book.add(1, Side::Sell, 100, 5));
    ASSERT_TRUE(this->book.add(2, Side::Sell, 101, 5));
    std::vector<Fill> fills;
    const auto result = this->book.match_limit(3, Side::Buy, 101, 8, 0, &fills);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.filled, 8u);
    EXPECT_FALSE(result.rested);
    EXPECT_EQ(this->book.order_count(), 1u);
    EXPECT_EQ(this->book.level_qty(Side::Sell, 101), 2u);
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[0].qty, 5u);
    EXPECT_EQ(fills[1].price, 101);
    EXPECT_EQ(fills[1].qty, 3u);
}

TYPED_TEST(MatchBook, FifoOnOneLevel) {
    ASSERT_TRUE(this->book.add(1, Side::Sell, 100, 4));
    ASSERT_TRUE(this->book.add(2, Side::Sell, 100, 6));
    std::vector<Fill> fills;
    const auto result = this->book.match_limit(3, Side::Buy, 100, 5, 0, &fills);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.filled, 5u);
    EXPECT_EQ(this->book.fifo_ids(Side::Sell, 100), (std::vector<fastlob::OrderId>{2}));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].maker_id, 1u);
    EXPECT_EQ(fills[0].qty, 4u);
    EXPECT_EQ(fills[1].maker_id, 2u);
    EXPECT_EQ(fills[1].qty, 1u);
}

TYPED_TEST(MatchBook, RemainderRestsOnTakerSide) {
    ASSERT_TRUE(this->book.add(1, Side::Sell, 100, 3));
    const auto result = this->book.match_limit(2, Side::Buy, 100, 10);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.filled, 3u);
    EXPECT_EQ(result.remaining, 7u);
    EXPECT_TRUE(result.rested);
    EXPECT_EQ(this->book.level_qty(Side::Buy, 100), 7u);
    EXPECT_FALSE(this->book.best_price(Side::Sell).has_value());
}

TYPED_TEST(MatchBook, MarketDoesNotRest) {
    ASSERT_TRUE(this->book.add(1, Side::Sell, 100, 2));
    ASSERT_TRUE(this->book.add(2, Side::Sell, 110, 2));
    const auto result = this->book.match_market(3, Side::Buy, 10);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.filled, 4u);
    EXPECT_EQ(result.remaining, 6u);
    EXPECT_FALSE(result.rested);
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(MatchBook, ApplyDoesNotMatch) {
    ASSERT_TRUE(this->book.add(1, Side::Sell, 100, 10));
    ASSERT_TRUE(this->book.add(2, Side::Buy, 100, 10));
    EXPECT_EQ(this->book.order_count(), 2u);
    EXPECT_EQ(this->book.level_qty(Side::Buy, 100), 10u);
    EXPECT_EQ(this->book.level_qty(Side::Sell, 100), 10u);
}

TYPED_TEST(MatchBook, CancelUsesLevelHandle) {
    ASSERT_TRUE(this->book.add(1, Side::Buy, 50, 4));
    ASSERT_TRUE(this->book.add(2, Side::Buy, 50, 6));
    ASSERT_TRUE(this->book.cancel(1));
    EXPECT_EQ(this->book.level_qty(Side::Buy, 50), 6u);
    ASSERT_TRUE(this->book.cancel(2));
    EXPECT_EQ(this->book.order_count(), 0u);
    EXPECT_FALSE(this->book.best_price(Side::Buy).has_value());
}

TEST(Match, PagedAndMapAgreeOnWalk) {
    PagedLOB paged(2048, 2048);
    MapStorageBook map_book(2048, 2048);
    for (int i = 0; i < 32; ++i) {
        ASSERT_TRUE(paged.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, 100 + i, 3));
        ASSERT_TRUE(map_book.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, 100 + i, 3));
    }
    std::vector<Fill> paged_fills;
    std::vector<Fill> map_fills;
    const auto paged_result = paged.match_limit(1000, Side::Buy, 120, 40, 0, &paged_fills);
    const auto map_result = map_book.match_limit(1000, Side::Buy, 120, 40, 0, &map_fills);
    EXPECT_EQ(paged_result.filled, map_result.filled);
    EXPECT_EQ(paged_result.remaining, map_result.remaining);
    EXPECT_EQ(paged_result.rested, map_result.rested);
    ASSERT_EQ(paged_fills.size(), map_fills.size());
    for (std::size_t i = 0; i < paged_fills.size(); ++i) {
        EXPECT_EQ(paged_fills[i].maker_id, map_fills[i].maker_id);
        EXPECT_EQ(paged_fills[i].price, map_fills[i].price);
        EXPECT_EQ(paged_fills[i].qty, map_fills[i].qty);
    }
    EXPECT_EQ(paged.best_price(Side::Sell), map_book.best_price(Side::Sell));
    EXPECT_EQ(paged.best_price(Side::Buy), map_book.best_price(Side::Buy));
}

TYPED_TEST(MatchBook, MarketEmptyAndDuplicateIdEdgeCases) {
    const auto empty_res = this->book.match_market(1, Side::Buy, 10);
    EXPECT_TRUE(empty_res.ok);
    EXPECT_EQ(empty_res.filled, 0u);
    EXPECT_EQ(empty_res.remaining, 10u);
    EXPECT_FALSE(empty_res.rested);

    ASSERT_TRUE(this->book.add(10, Side::Sell, 100, 5));
    const auto dup_limit = this->book.match_limit(10, Side::Buy, 100, 3);
    EXPECT_FALSE(dup_limit.ok);
    EXPECT_EQ(this->book.level_qty(Side::Sell, 100), 5u);

    const auto dup_market = this->book.match_market(10, Side::Buy, 3);
    EXPECT_FALSE(dup_market.ok);
    EXPECT_EQ(this->book.level_qty(Side::Sell, 100), 5u);
}

TEST(Match, TightCapacityPartialAndDrainRest) {
    PagedLOB tight(1, 1);
    ASSERT_TRUE(tight.add(1, Side::Sell, 100, 10));

    // Partial fill of maker: remaining == 0, does not need to rest even at 0 free slots.
    const auto partial = tight.match_limit(2, Side::Buy, 100, 4);
    EXPECT_TRUE(partial.ok);
    EXPECT_EQ(partial.filled, 4u);
    EXPECT_EQ(partial.remaining, 0u);
    EXPECT_FALSE(partial.rested);
    EXPECT_EQ(tight.level_qty(Side::Sell, 100), 6u);

    // Non-crossing limit when OrderPool is full is rejected cleanly without mutation.
    const auto rejected = tight.match_limit(3, Side::Buy, 95, 2);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.filled, 0u);
    EXPECT_EQ(tight.level_qty(Side::Sell, 100), 6u);

    // Over-filling drains Sell@100 (freeing the 1 Order + 1 BookLevel), then rests remaining 5.
    const auto drain_rest = tight.match_limit(4, Side::Buy, 100, 11);
    EXPECT_TRUE(drain_rest.ok);
    EXPECT_EQ(drain_rest.filled, 6u);
    EXPECT_EQ(drain_rest.remaining, 5u);
    EXPECT_TRUE(drain_rest.rested);
    EXPECT_EQ(tight.level_qty(Side::Buy, 100), 5u);
}

TEST(Match, FindScrubsInternalLinksAndFullIdIndexTerminates) {
    PagedLOB book(16, 8);
    ASSERT_TRUE(book.add(1, Side::Buy, 100, 5));
    ASSERT_TRUE(book.add(2, Side::Buy, 100, 7));
    const auto snap = book.find(1);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->qty, 5u);
    EXPECT_EQ(snap->prev, nullptr);
    EXPECT_EQ(snap->next, nullptr);
    EXPECT_EQ(snap->level_handle, 0u);

    fastlob::OrderIdIndex idx(16);
    fastlob::Order dummy{};
    for (fastlob::OrderId id = 1; id <= 16; ++id) {
        EXPECT_TRUE(idx.insert(id, &dummy));
    }
    std::size_t slot = 0;
    EXPECT_FALSE(idx.find_empty_slot(99, slot));
    EXPECT_FALSE(idx.insert(99, &dummy));
    EXPECT_EQ(idx.find(99), nullptr);
    EXPECT_EQ(idx.take(8), &dummy);
    EXPECT_EQ(idx.find(8), nullptr);
    EXPECT_EQ(idx.find(16), &dummy);
}
