#include "fastlob/lob_reference.hpp"

#include <gtest/gtest.h>

using fastlob::Price;
using fastlob::Qty;
using fastlob::ReferenceOrderBook;
using fastlob::Side;
using fastlob::dollars_to_ticks;

TEST(ReferenceLob, AddCancelFifo) {
    ReferenceOrderBook book;
    const Price px = dollars_to_ticks(100.0);
    ASSERT_TRUE(book.add(1, Side::Buy, px, 100));
    ASSERT_TRUE(book.add(2, Side::Buy, px, 200));
    EXPECT_EQ(book.level_qty(Side::Buy, px), 300u);
    auto ids = book.fifo_ids(Side::Buy, px);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 1u);
    EXPECT_EQ(ids[1], 2u);
    ASSERT_TRUE(book.cancel(1));
    ids = book.fifo_ids(Side::Buy, px);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 2u);
    EXPECT_EQ(book.level_qty(Side::Buy, px), 200u);
}

TEST(ReferenceLob, ModifyDecreaseKeepsPriority) {
    ReferenceOrderBook book;
    const Price px = dollars_to_ticks(100.0);
    ASSERT_TRUE(book.add(1, Side::Buy, px, 100));
    ASSERT_TRUE(book.add(2, Side::Buy, px, 200));
    ASSERT_TRUE(book.modify(1, 50));
    EXPECT_FALSE(book.modify(1, 80));  // increase forbidden
    auto ids = book.fifo_ids(Side::Buy, px);
    EXPECT_EQ(ids[0], 1u);
    EXPECT_EQ(book.find(1)->qty, 50u);
}

TEST(ReferenceLob, ExecutePartialAndFull) {
    ReferenceOrderBook book;
    const Price px = dollars_to_ticks(100.0);
    ASSERT_TRUE(book.add(1, Side::Sell, px, 100));
    ASSERT_TRUE(book.execute(1, 40));
    EXPECT_EQ(book.find(1)->qty, 60u);
    ASSERT_TRUE(book.execute(1, 60));
    EXPECT_FALSE(book.find(1).has_value());
}

TEST(ReferenceLob, TopN) {
    ReferenceOrderBook book;
    ASSERT_TRUE(book.add(1, Side::Buy, dollars_to_ticks(99.0), 10));
    ASSERT_TRUE(book.add(2, Side::Buy, dollars_to_ticks(100.0), 20));
    ASSERT_TRUE(book.add(3, Side::Sell, dollars_to_ticks(101.0), 30));
    ASSERT_TRUE(book.add(4, Side::Sell, dollars_to_ticks(102.0), 40));
    fastlob::LevelView out[4];
    const auto nb = book.top_n(Side::Buy, out);
    ASSERT_EQ(nb, 2u);
    EXPECT_EQ(out[0].price, dollars_to_ticks(100.0));
    EXPECT_EQ(out[0].qty, 20u);
    const auto na = book.top_n(Side::Sell, out);
    ASSERT_EQ(na, 2u);
    EXPECT_EQ(out[0].price, dollars_to_ticks(101.0));
}
