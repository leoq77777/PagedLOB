#include "fastlob/storage_book.hpp"

#include <gtest/gtest.h>

#include <vector>

using fastlob::MapStorageBook;
using fastlob::PagedLOB;
using fastlob::Price;
using fastlob::Qty;
using fastlob::Side;

TEST(HotCold, NearBestStaysOffTheRadix) {
    constexpr int kHot = fastlob::PagedPriceIndex<>::kHotTicks;
    PagedLOB book(2048, 2048);
    for (int i = 0; i < kHot; ++i) {
        ASSERT_TRUE(book.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, i, 1));
    }
    EXPECT_EQ(book.page_stats().live_leaves, 0u);
    EXPECT_EQ(book.best_price(Side::Sell), 0);
    EXPECT_EQ(book.active_levels(Side::Sell), static_cast<std::size_t>(kHot));
}

TEST(HotCold, FarLevelsUsePages) {
    constexpr int kHot = fastlob::PagedPriceIndex<>::kHotTicks;
    PagedLOB book(2048, 2048);
    for (int i = 0; i < kHot + 64; ++i) {
        ASSERT_TRUE(book.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, i, 1));
    }
    EXPECT_EQ(book.page_stats().live_leaves, 1u);
    EXPECT_EQ(book.level_qty(Side::Sell, kHot + 8), 1u);

    ASSERT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_price(Side::Sell), 1);
    EXPECT_EQ(book.page_stats().live_leaves, 1u);
}

TEST(HotCold, PromoteIntoWindowOnBestMove) {
    constexpr int kHot = fastlob::PagedPriceIndex<>::kHotTicks;
    PagedLOB book(2048, 2048);
    for (int i = 0; i < kHot + 16; ++i) {
        ASSERT_TRUE(book.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, i, 1));
    }
    EXPECT_EQ(book.page_stats().live_leaves, 1u);
    for (int i = 0; i < 16; ++i) {
        ASSERT_TRUE(book.cancel(static_cast<fastlob::OrderId>(i + 1)));
    }
    EXPECT_EQ(book.best_price(Side::Sell), 16);
    EXPECT_EQ(book.page_stats().live_leaves, 0u);
    EXPECT_EQ(book.level_qty(Side::Sell, kHot + 15), 1u);
}

TEST(HotCold, JumpBetterRebuildsWindow) {
    PagedLOB book(64, 64);
    ASSERT_TRUE(book.add(1, Side::Buy, 100, 2));
    ASSERT_TRUE(book.add(2, Side::Buy, 90, 3));
    ASSERT_TRUE(book.add(3, Side::Buy, 1000, 4));
    EXPECT_EQ(book.best_price(Side::Buy), 1000);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 2u);
    EXPECT_EQ(book.level_qty(Side::Buy, 90), 3u);
    std::vector<fastlob::LevelView> top(8);
    ASSERT_EQ(book.top_n(Side::Buy, top), 3u);
    EXPECT_EQ(top[0].price, 1000);
    EXPECT_EQ(top[1].price, 100);
    EXPECT_EQ(top[2].price, 90);
}

TEST(HotCold, MatchWalksHotThenCold) {
    constexpr int kHot = fastlob::PagedPriceIndex<>::kHotTicks;
    PagedLOB paged(2048, 2048);
    MapStorageBook map_book(2048, 2048);
    const int depth = kHot + 32;
    for (int i = 0; i < depth; ++i) {
        ASSERT_TRUE(paged.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, 1000 + i, 1));
        ASSERT_TRUE(map_book.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, 1000 + i, 1));
    }
    std::vector<fastlob::Fill> paged_fills;
    std::vector<fastlob::Fill> map_fills;
    const auto paged_result = paged.match_limit(9000, Side::Buy, 1000 + depth - 1, static_cast<Qty>(depth),
                                                0, &paged_fills);
    const auto map_result = map_book.match_limit(9000, Side::Buy, 1000 + depth - 1, static_cast<Qty>(depth),
                                                 0, &map_fills);
    EXPECT_EQ(paged_result.filled, map_result.filled);
    EXPECT_EQ(paged_result.filled, static_cast<Qty>(depth));
    EXPECT_FALSE(paged_result.rested);
    ASSERT_EQ(paged_fills.size(), map_fills.size());
    for (std::size_t i = 0; i < paged_fills.size(); ++i) {
        EXPECT_EQ(paged_fills[i].price, map_fills[i].price);
        EXPECT_EQ(paged_fills[i].qty, map_fills[i].qty);
    }
    EXPECT_EQ(paged.order_count(), 0u);
    EXPECT_EQ(paged.page_stats().live_leaves, 0u);
}

TEST(HotCold, FailedBetterOpenRollsBack) {
    PagedLOB tight(1, 8);
    ASSERT_TRUE(tight.add(1, Side::Buy, 10, 1));
    EXPECT_FALSE(tight.add(2, Side::Buy, 10 + 5000, 1));
    EXPECT_EQ(tight.order_count(), 1u);
    EXPECT_EQ(tight.best_price(Side::Buy), 10);
    EXPECT_EQ(tight.level_qty(Side::Buy, 10), 1u);
    EXPECT_EQ(tight.page_stats().live_leaves, 0u);
}
