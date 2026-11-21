#include "fastlob/execution_sim.hpp"
#include "fastlob/lob_fast.hpp"

#include <gtest/gtest.h>

using fastlob::ExecutionSimulator;
using fastlob::FastOrderBook;
using fastlob::Side;
using fastlob::dollars_to_ticks;

/// Tutorial §3.1: ahead 1000, trades 400 then 700 => fill 100 @ 100.00
TEST(ExecutionSim, ShadowQueueTutorialExample) {
    FastOrderBook book(1024, 64);
    const auto px = dollars_to_ticks(100.0);
    // Public resting liquidity totaling 1000 at bid
    ASSERT_TRUE(book.add(1, Side::Buy, px, 1000));

    ExecutionSimulator sim(book);
    ASSERT_TRUE(sim.submit_limit(9001, Side::Buy, px, 100));
    EXPECT_FALSE(sim.shadow_in_public_book(9001));
    ASSERT_NE(sim.shadow(9001), nullptr);
    EXPECT_EQ(sim.shadow(9001)->ahead_qty, 1000u);

    // Public trade consumes 400 from ahead
    ASSERT_TRUE(book.execute(1, 400));
    sim.on_public_trade(Side::Buy, px, 400);
    EXPECT_EQ(sim.shadow(9001)->ahead_qty, 600u);
    EXPECT_TRUE(sim.fills().empty());

    // Trade 700: 600 ahead + 100 strategy fill
    ASSERT_TRUE(book.execute(1, 600));
    // book order fully gone; remaining trade qty against shadow only
    sim.on_public_trade(Side::Buy, px, 700);
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_EQ(sim.fills()[0].qty, 100u);
    EXPECT_EQ(sim.fills()[0].price, px);
    EXPECT_TRUE(sim.fills()[0].fully_filled);
    EXPECT_FALSE(sim.shadow(9001)->active);
}

/// Tutorial §3.2: market buy 250 -> VWAP 100.16
TEST(ExecutionSim, MarketSlippageTutorialExample) {
    FastOrderBook book(1024, 64);
    ASSERT_TRUE(book.add(1, Side::Sell, dollars_to_ticks(100.10), 100));
    ASSERT_TRUE(book.add(2, Side::Sell, dollars_to_ticks(100.20), 200));

    ExecutionSimulator sim(book);
    auto fill = sim.submit_market(8001, Side::Buy, 250);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->qty, 250u);
    EXPECT_EQ(fill->price, dollars_to_ticks(100.16));
    EXPECT_TRUE(fill->fully_filled);
    // Public book unchanged
    EXPECT_EQ(book.level_qty(Side::Sell, dollars_to_ticks(100.10)), 100u);
}

TEST(ExecutionSim, ShadowNeverInPublicFifo) {
    FastOrderBook book(256, 32);
    const auto px = dollars_to_ticks(50.0);
    ASSERT_TRUE(book.add(1, Side::Buy, px, 10));
    ExecutionSimulator sim(book);
    ASSERT_TRUE(sim.submit_limit(42, Side::Buy, px, 5));
    auto ids = book.fifo_ids(Side::Buy, px);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 1u);
    EXPECT_FALSE(sim.shadow_in_public_book(42));
}

TEST(ExecutionSim, WorksWithPagedLOBDefault) {
    fastlob::PagedLOB book(1024, 64);
    const auto px = dollars_to_ticks(100.0);
    ASSERT_TRUE(book.add(1, Side::Buy, px, 500));
    ASSERT_TRUE(book.add(2, Side::Sell, dollars_to_ticks(100.10), 100));
    ASSERT_TRUE(book.add(3, Side::Sell, dollars_to_ticks(100.20), 200));

    ExecutionSimulator sim(book);
    ASSERT_TRUE(sim.submit_limit(9001, Side::Buy, px, 100));
    ASSERT_TRUE(book.execute(1, 500));
    sim.on_public_trade(Side::Buy, px, 600);
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_EQ(sim.fills()[0].qty, 100u);

    auto mkt = sim.submit_market(8001, Side::Buy, 250);
    ASSERT_TRUE(mkt.has_value());
    EXPECT_EQ(mkt->qty, 250u);
    EXPECT_EQ(mkt->price, dollars_to_ticks(100.16));
}
