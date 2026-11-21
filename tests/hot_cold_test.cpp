#include "fastlob/storage_book.hpp"

#include <gtest/gtest.h>

#include <vector>

using fastlob::MapStorageBook;
using fastlob::PagedLOB;
using fastlob::Price;
using fastlob::Qty;
using fastlob::Side;

TEST(HotCold, NearBestStaysInSingleLeaf) {
    constexpr int kHot = fastlob::PagedPriceIndex<>::kHotTicks;
    PagedLOB book(2048, 2048);
    for (int i = 0; i < kHot; ++i) {
        ASSERT_TRUE(book.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, i, 1));
    }
    EXPECT_EQ(book.page_stats().live_leaves, 1u);
    EXPECT_EQ(book.best_price(Side::Sell), 0);
    EXPECT_EQ(book.active_levels(Side::Sell), static_cast<std::size_t>(kHot));
}

TEST(HotCold, FarLevelsUsePages) {
    constexpr int kHot = fastlob::PagedPriceIndex<>::kHotTicks;
    PagedLOB book(2048, 2048);
    for (int i = 0; i < kHot + 64; ++i) {
        ASSERT_TRUE(book.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, i, 1));
    }
    EXPECT_EQ(book.page_stats().live_leaves, 2u);
    EXPECT_EQ(book.level_qty(Side::Sell, kHot + 8), 1u);

    ASSERT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_price(Side::Sell), 1);
    EXPECT_EQ(book.page_stats().live_leaves, 2u);
}

TEST(HotCold, PromoteIntoWindowOnBestMove) {
    constexpr int kHot = fastlob::PagedPriceIndex<>::kHotTicks;
    PagedLOB book(2048, 2048);
    for (int i = 0; i < kHot + 16; ++i) {
        ASSERT_TRUE(book.add(static_cast<fastlob::OrderId>(i + 1), Side::Sell, i, 1));
    }
    EXPECT_EQ(book.page_stats().live_leaves, 2u);
    for (int i = 0; i < 16; ++i) {
        ASSERT_TRUE(book.cancel(static_cast<fastlob::OrderId>(i + 1)));
    }
    EXPECT_EQ(book.best_price(Side::Sell), 16);
    EXPECT_EQ(book.page_stats().live_leaves, 2u);
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
    EXPECT_EQ(tight.page_stats().live_leaves, 1u);
}

TEST(HotCold, ZeroMigrationUnderAdversarialSpikes) {
    PagedLOB book(4096, 4096);
    book.reserve_cold(16, 16);
    // Populate 200 dense resting Buy levels at [10'000 .. 10'199]
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(book.add(static_cast<fastlob::OrderId>(i + 1), Side::Buy, 10'000 + i, 5));
    }
    const auto baseline = book.page_stats();
    // Repeatedly insert & cancel transient spikes +15,000 ticks ahead of best bid
    for (int iter = 0; iter < 2000; ++iter) {
        const fastlob::OrderId spike_id = 50'000 + static_cast<fastlob::OrderId>(iter);
        const Price spike_px = 25'000 + (iter % 512);
        ASSERT_TRUE(book.add(spike_id, Side::Buy, spike_px, 1));
        EXPECT_EQ(book.best_price(Side::Buy), spike_px);
        ASSERT_TRUE(book.cancel(spike_id));
        EXPECT_EQ(book.best_price(Side::Buy), 10'199);
    }
    const auto after = book.page_stats();
    EXPECT_EQ(after.committed_nodes, baseline.committed_nodes);
    EXPECT_EQ(after.committed_leaves, baseline.committed_leaves);
    EXPECT_EQ(after.live_leaves, baseline.live_leaves);
    EXPECT_EQ(book.level_qty(Side::Buy, 10'000), 5u);
    EXPECT_EQ(book.level_qty(Side::Buy, 10'199), 5u);
}

TEST(HotCold, Int64ExtremesAndLargeLevelQty) {
    const Price max_px = std::numeric_limits<Price>::max();
    const Price min_px = std::numeric_limits<Price>::min();
    PagedLOB book(64, 64);

    // Hot window anchored near INT64_MAX on Sell and INT64_MIN on Buy without signed overflow.
    ASSERT_TRUE(book.add(1, Side::Sell, max_px - 10, 3));
    ASSERT_TRUE(book.add(2, Side::Sell, max_px, 4));
    ASSERT_TRUE(book.add(3, Side::Buy, min_px + 10, 5));
    ASSERT_TRUE(book.add(4, Side::Buy, min_px, 6));
    EXPECT_EQ(book.best_price(Side::Sell), max_px - 10);
    EXPECT_EQ(book.best_price(Side::Buy), min_px + 10);
    ASSERT_TRUE(book.cancel(1));
    ASSERT_TRUE(book.cancel(3));
    EXPECT_EQ(book.best_price(Side::Sell), max_px);
    EXPECT_EQ(book.best_price(Side::Buy), min_px);

    // Aggregate level quantity exceeding UINT32_MAX is preserved in 64-bit LevelQty / LevelView.
    const Qty big = 3'000'000'000u;
    ASSERT_TRUE(book.add(10, Side::Buy, 500, big));
    ASSERT_TRUE(book.add(11, Side::Buy, 500, big));
    const fastlob::LevelQty expected = static_cast<fastlob::LevelQty>(big) * 2;
    EXPECT_EQ(book.level_qty(Side::Buy, 500), expected);
    fastlob::LevelView top[2];
    ASSERT_GE(book.top_n(Side::Buy, top), 1u);
    EXPECT_EQ(top[0].price, 500);
    EXPECT_EQ(top[0].qty, expected);
}

TEST(HotCold, ColdMiddleLeafOrderPreserved) {
    PagedLOB book(64, 64);
    ASSERT_TRUE(book.add(1, Side::Sell, 0, 1));       // hot anchor
    ASSERT_TRUE(book.add(2, Side::Sell, 10'000, 2));  // far cold max leaf
    ASSERT_TRUE(book.add(3, Side::Sell, 1'000, 3));   // cold min leaf
    ASSERT_TRUE(book.add(4, Side::Sell, 5'000, 4));   // middle cold leaf (via tree_successor)
    std::vector<fastlob::LevelView> out(8);
    ASSERT_EQ(book.top_n(Side::Sell, out), 4u);
    EXPECT_EQ(out[0].price, 0);
    EXPECT_EQ(out[1].price, 1'000);
    EXPECT_EQ(out[2].price, 5'000);
    EXPECT_EQ(out[3].price, 10'000);
}
