#include "fastlob/lob_fast.hpp"

#include <gtest/gtest.h>

using fastlob::FastOrderBook;
using fastlob::Side;
using fastlob::dollars_to_ticks;

TEST(FastLob, BasicOps) {
    FastOrderBook book(1024, 256);
    const auto px = dollars_to_ticks(100.0);
    ASSERT_TRUE(book.add(1, Side::Buy, px, 100));
    ASSERT_TRUE(book.add(2, Side::Buy, px, 200));
    EXPECT_EQ(book.level_qty(Side::Buy, px), 300u);
    ASSERT_TRUE(book.cancel(1));
    EXPECT_EQ(book.level_qty(Side::Buy, px), 200u);
    ASSERT_TRUE(book.modify(2, 50));
    EXPECT_EQ(book.find(2)->qty, 50u);
    ASSERT_TRUE(book.execute(2, 50));
    EXPECT_EQ(book.order_count(), 0u);
}
