#include "fastlob/order.hpp"

#include <gtest/gtest.h>

TEST(OrderLayout, FitsCacheLineAndCompact) {
    EXPECT_LE(sizeof(fastlob::Order), 64u);
    EXPECT_LE(alignof(fastlob::Order), 8u);
}
