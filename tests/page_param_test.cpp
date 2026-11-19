#include "fastlob/lob_reference.hpp"
#include "fastlob/price_key.hpp"
#include "fastlob/storage_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

namespace {

template <int PageBits>
std::size_t cold_leaves_at(const std::vector<fastlob::Price>& bids, const std::vector<fastlob::Price>& asks) {
    constexpr int kHot = fastlob::PagedPriceIndex<PageBits>::kHotTicks;
    auto count = [](fastlob::Side side, const std::vector<fastlob::Price>& prices) {
        if (prices.empty()) {
            return std::size_t{0};
        }
        fastlob::Price best = prices.front();
        for (fastlob::Price price : prices) {
            if (side == fastlob::Side::Buy ? price > best : price < best) {
                best = price;
            }
        }
        std::set<std::uint64_t> pages;
        for (fastlob::Price price : prices) {
            const bool hot =
                side == fastlob::Side::Sell
                    ? (price >= best && static_cast<std::uint64_t>(price) -
                                                static_cast<std::uint64_t>(best) <
                                            static_cast<std::uint64_t>(kHot))
                    : (price <= best && static_cast<std::uint64_t>(best) -
                                                static_cast<std::uint64_t>(price) <
                                            static_cast<std::uint64_t>(kHot));
            if (!hot) {
                pages.insert(fastlob::tick_page_key<PageBits>(price));
            }
        }
        return pages.size();
    };
    return count(fastlob::Side::Buy, bids) + count(fastlob::Side::Sell, asks);
}

template <int PageBits>
void check_page_width() {
    using Book = fastlob::PagedStorageBookT<PageBits>;
    constexpr int kTicks = 1 << PageBits;
    static_assert(fastlob::PagedPriceIndex<PageBits>::kTicks == kTicks);

    EXPECT_EQ(fastlob::tick_page_key<PageBits>(0), fastlob::tick_page_key<PageBits>(kTicks - 1));
    EXPECT_NE(fastlob::tick_page_key<PageBits>(0), fastlob::tick_page_key<PageBits>(kTicks));
    EXPECT_EQ(fastlob::tick_page_slot<PageBits>(0), 0);
    EXPECT_EQ(fastlob::tick_page_slot<PageBits>(kTicks - 1), kTicks - 1);
    EXPECT_EQ(fastlob::tick_page_slot<PageBits>(kTicks), 0);

    for (int slot : {0, 1, kTicks - 1}) {
        const auto price = fastlob::tick_from_page<PageBits>(3, slot);
        EXPECT_EQ(fastlob::tick_page_key<PageBits>(price), 3u);
        EXPECT_EQ(fastlob::tick_page_slot<PageBits>(price), slot);
    }

    constexpr int kLevels = kTicks * 3;
    const std::size_t cap = static_cast<std::size_t>(kLevels) * 2 + 16;
    Book paged(cap, cap);
    fastlob::MapStorageBook map_book(cap, cap);
    fastlob::ReferenceOrderBook reference;

    auto add = [&](fastlob::OrderId id, fastlob::Side side, fastlob::Price price) {
        fastlob::BookEvent event;
        event.type = fastlob::EventType::Add;
        event.order_id = id;
        event.side = side;
        event.price = price;
        event.qty = 1;
        EXPECT_TRUE(reference.apply(event));
        EXPECT_TRUE(map_book.apply(event));
        EXPECT_TRUE(paged.apply(event));
    };
    auto cancel = [&](fastlob::OrderId id) {
        fastlob::BookEvent event;
        event.type = fastlob::EventType::Cancel;
        event.order_id = id;
        EXPECT_TRUE(reference.apply(event));
        EXPECT_TRUE(map_book.apply(event));
        EXPECT_TRUE(paged.apply(event));
    };

    std::vector<fastlob::Price> sells;
    std::vector<fastlob::Price> buys;
    for (int i = 0; i < kLevels; ++i) {
        add(static_cast<fastlob::OrderId>(i + 1), fastlob::Side::Sell, i);
        add(static_cast<fastlob::OrderId>(kLevels + i + 1), fastlob::Side::Buy, -1 - i);
        sells.push_back(i);
        buys.push_back(-1 - i);
    }
    EXPECT_EQ(paged.page_stats().live_leaves, cold_leaves_at<PageBits>(buys, sells));

    std::vector<fastlob::LevelView> ref_out(static_cast<std::size_t>(kLevels));
    std::vector<fastlob::LevelView> paged_out(static_cast<std::size_t>(kLevels));
    std::vector<fastlob::LevelView> map_out(static_cast<std::size_t>(kLevels));
    const auto n_ask = reference.top_n(fastlob::Side::Sell, ref_out);
    EXPECT_EQ(paged.top_n(fastlob::Side::Sell, paged_out), n_ask);
    EXPECT_EQ(map_book.top_n(fastlob::Side::Sell, map_out), n_ask);
    for (std::size_t i = 0; i < n_ask; ++i) {
        EXPECT_EQ(paged_out[i].price, ref_out[i].price) << "ask " << i << " page_bits=" << PageBits;
        EXPECT_EQ(map_out[i].price, ref_out[i].price);
        EXPECT_EQ(paged_out[i].qty, ref_out[i].qty);
    }

    const auto n_bid = reference.top_n(fastlob::Side::Buy, ref_out);
    EXPECT_EQ(paged.top_n(fastlob::Side::Buy, paged_out), n_bid);
    for (std::size_t i = 0; i < n_bid; ++i) {
        EXPECT_EQ(paged_out[i].price, ref_out[i].price) << "bid " << i << " page_bits=" << PageBits;
    }

    for (int i = 0; i < kTicks; ++i) {
        cancel(static_cast<fastlob::OrderId>(i + 1));
    }
    sells.erase(sells.begin(), sells.begin() + kTicks);
    EXPECT_EQ(paged.page_stats().live_leaves, cold_leaves_at<PageBits>(buys, sells));
    EXPECT_EQ(paged.top_n(fastlob::Side::Sell, paged_out),
              reference.top_n(fastlob::Side::Sell, ref_out));
    EXPECT_EQ(paged_out[0].price, kTicks);

    for (int i = kTicks; i < kLevels; ++i) {
        cancel(static_cast<fastlob::OrderId>(i + 1));
        cancel(static_cast<fastlob::OrderId>(kLevels + i + 1));
    }
    for (int i = 0; i < kTicks; ++i) {
        cancel(static_cast<fastlob::OrderId>(kLevels + i + 1));
    }
    EXPECT_EQ(paged.order_count(), 0u);
    EXPECT_EQ(paged.page_stats().live_leaves, 0u);
}

}  // namespace

TEST(PageParam, Width32) { check_page_width<5>(); }
TEST(PageParam, Width64) { check_page_width<6>(); }
TEST(PageParam, Width128) { check_page_width<7>(); }
TEST(PageParam, Width256) { check_page_width<8>(); }
TEST(PageParam, Width512) { check_page_width<9>(); }
TEST(PageParam, Width1024) { check_page_width<10>(); }

TEST(PageParam, DefaultIs256) {
    EXPECT_EQ(fastlob::PagedPriceIndex<8>::kTicks, 256);
    EXPECT_EQ(fastlob::PagedPriceIndex<>::kTicks, 256);
    EXPECT_EQ(fastlob::PagedPriceIndex<8>::kLastDirDepth, 6);
    EXPECT_EQ(fastlob::PagedPriceIndex<8>::kHotTicks, 256);
}
