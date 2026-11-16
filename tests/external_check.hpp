#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

template <typename Left, typename Right>
void expect_books_agree(const char* label) {
    Left left(128, 128);
    Right right(128, 128);
    struct Event {
        bool is_add;
        std::uint64_t id;
        bool buy;
        std::int64_t price;
        std::uint32_t qty;
    };
    const Event events[] = {
        {true, 1, true, 100, 5},  {true, 2, true, 100, 4}, {true, 3, true, 90, 8},
        {true, 4, false, 120, 6}, {true, 5, false, 130, 2}, {false, 2, true, 0, 0},
        {true, 6, true, 95, 1},   {true, 7, false, 125, 3},
    };
    for (const Event& event : events) {
        if (event.is_add) {
            ASSERT_TRUE(left.add(event.id, event.buy, event.price, event.qty)) << label;
            ASSERT_TRUE(right.add(event.id, event.buy, event.price, event.qty)) << label;
        } else {
            ASSERT_TRUE(left.cancel(event.id)) << label;
            ASSERT_TRUE(right.cancel(event.id)) << label;
        }
    }
    for (bool buy : {true, false}) {
        std::vector<typename Left::Level> a;
        std::vector<typename Right::Level> b;
        left.collect(buy, a);
        right.collect(buy, b);
        ASSERT_EQ(a.size(), b.size()) << label << " side=" << buy;
        for (std::size_t i = 0; i < a.size(); ++i) {
            EXPECT_EQ(a[i].price, b[i].price) << label << " i=" << i;
            EXPECT_EQ(a[i].qty, b[i].qty) << label << " i=" << i;
        }
    }
}
