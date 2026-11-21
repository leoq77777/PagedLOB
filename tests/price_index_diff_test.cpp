#include "fastlob/lob_fast.hpp"
#include "fastlob/lob_reference.hpp"
#include "fastlob/price_key.hpp"
#include "fastlob/storage_book.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <tuple>
#include <type_traits>
#include <vector>

static_assert(std::is_same_v<fastlob::PagedLOB, fastlob::PagedStorageBook>);

using fastlob::BookEvent;
using fastlob::EventType;
using fastlob::FastOrderBook;
using fastlob::LevelView;
using fastlob::MapStorageBook;
using fastlob::OrderId;
using fastlob::PagedStorageBook;
using fastlob::Price;
using fastlob::Qty;
using fastlob::ReferenceOrderBook;
using fastlob::Side;
using fastlob::tick_page_key;

namespace {

struct Books {
    ReferenceOrderBook reference;
    FastOrderBook v1;
    MapStorageBook map_book;
    PagedStorageBook paged;

    Books(std::size_t orders, std::size_t levels)
        : v1(orders, levels), map_book(orders, levels), paged(orders, levels) {}

    bool apply(const BookEvent& event) {
        const bool ok_ref = reference.apply(event);
        const bool ok_v1 = v1.apply(event);
        const bool ok_map = map_book.apply(event);
        const bool ok_paged = paged.apply(event);
        EXPECT_EQ(ok_ref, ok_v1) << "v1 status";
        EXPECT_EQ(ok_ref, ok_map) << "map status";
        EXPECT_EQ(ok_ref, ok_paged) << "paged status";
        return ok_ref && ok_v1 && ok_map && ok_paged;
    }

    bool apply_without_v1(const BookEvent& event) {
        const bool ok_ref = reference.apply(event);
        const bool ok_map = map_book.apply(event);
        const bool ok_paged = paged.apply(event);
        EXPECT_EQ(ok_ref, ok_map);
        EXPECT_EQ(ok_ref, ok_paged);
        return ok_ref && ok_map && ok_paged;
    }
};

bool same_top(const std::vector<LevelView>& lhs, std::size_t n_lhs, const std::vector<LevelView>& rhs,
              std::size_t n_rhs) {
    if (n_lhs != n_rhs) {
        return false;
    }
    for (std::size_t i = 0; i < n_lhs; ++i) {
        if (lhs[i].price != rhs[i].price || lhs[i].qty != rhs[i].qty) {
            return false;
        }
    }
    return true;
}

bool expect_match(const char* where, const Books& books, const std::vector<OrderId>& live,
                  bool include_v1 = true) {
    bool ok = true;
    const auto ref_count = books.reference.order_count();
    if (include_v1 && books.v1.order_count() != ref_count) {
        ADD_FAILURE() << where << " v1 order_count";
        ok = false;
    }
    if (books.map_book.order_count() != ref_count || books.paged.order_count() != ref_count) {
        ADD_FAILURE() << where << " storage order_count ref=" << ref_count
                      << " map=" << books.map_book.order_count()
                      << " paged=" << books.paged.order_count();
        ok = false;
    }

    std::vector<LevelView> ref_levels(4096);
    std::vector<LevelView> v1_levels(4096);
    std::vector<LevelView> map_levels(4096);
    std::vector<LevelView> paged_levels(4096);
    for (Side side : {Side::Buy, Side::Sell}) {
        const auto n_ref = books.reference.top_n(side, ref_levels);
        const auto n_map = books.map_book.top_n(side, map_levels);
        const auto n_paged = books.paged.top_n(side, paged_levels);
        if (!same_top(ref_levels, n_ref, map_levels, n_map) ||
            !same_top(ref_levels, n_ref, paged_levels, n_paged)) {
            ADD_FAILURE() << where << " top side=" << static_cast<int>(side) << " nref=" << n_ref
                          << " nmap=" << n_map << " npaged=" << n_paged;
            ok = false;
            continue;
        }
        if (include_v1) {
            const auto n_v1 = books.v1.top_n(side, v1_levels);
            if (!same_top(ref_levels, n_ref, v1_levels, n_v1)) {
                ADD_FAILURE() << where << " v1 top side=" << static_cast<int>(side);
                ok = false;
                continue;
            }
        }
        if (n_ref == ref_levels.size()) {
            ADD_FAILURE() << where << " top buffer filled; comparison is incomplete";
            ok = false;
        }
        for (std::size_t i = 0; i < n_ref; ++i) {
            const Price price = ref_levels[i].price;
            const auto ref_ids = books.reference.fifo_ids(side, price);
            const auto map_ids = books.map_book.fifo_ids(side, price);
            const auto paged_ids = books.paged.fifo_ids(side, price);
            if (ref_ids != map_ids || ref_ids != paged_ids) {
                ADD_FAILURE() << where << " fifo side=" << static_cast<int>(side) << " px=" << price;
                ok = false;
            }
            if (include_v1 && books.v1.fifo_ids(side, price) != ref_ids) {
                ADD_FAILURE() << where << " v1 fifo px=" << price;
                ok = false;
            }
            if (books.reference.level_qty(side, price) != books.map_book.level_qty(side, price) ||
                books.reference.level_qty(side, price) != books.paged.level_qty(side, price)) {
                ADD_FAILURE() << where << " level qty px=" << price;
                ok = false;
            }
        }
    }

    for (OrderId id : live) {
        const auto ref = books.reference.find(id);
        const auto map_order = books.map_book.find(id);
        const auto paged_order = books.paged.find(id);
        if (!ref || !map_order || !paged_order || ref->qty != map_order->qty ||
            ref->qty != paged_order->qty || ref->price != map_order->price ||
            ref->price != paged_order->price || ref->side != map_order->side ||
            ref->side != paged_order->side) {
            ADD_FAILURE() << where << " order " << id;
            ok = false;
            break;
        }
        if (include_v1) {
            const auto v1_order = books.v1.find(id);
            if (!v1_order || v1_order->qty != ref->qty || v1_order->price != ref->price ||
                v1_order->side != ref->side) {
                ADD_FAILURE() << where << " v1 order " << id;
                ok = false;
                break;
            }
        }
    }
    return ok;
}

std::size_t live_pages(const std::vector<std::tuple<OrderId, Side, Price, Qty>>& live) {
    std::set<std::uint64_t> bids;
    std::set<std::uint64_t> asks;
    for (const auto& [id, side, price, qty] : live) {
        (void)id;
        (void)qty;
        (side == Side::Buy ? bids : asks).insert(tick_page_key(price));
    }
    return bids.size() + asks.size();
}

}  // namespace

TEST(PriceIndex, EncodeRoundTripAndOrder) {
    const Price samples[] = {std::numeric_limits<Price>::min(),
                             std::numeric_limits<Price>::min() + 255,
                             std::numeric_limits<Price>::min() + 256,
                             -100000,
                             -256,
                             -1,
                             0,
                             1,
                             255,
                             256,
                             100000,
                             std::numeric_limits<Price>::max()};
    for (std::size_t i = 1; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        EXPECT_LT(fastlob::encode_tick(samples[i - 1]), fastlob::encode_tick(samples[i]));
    }
    for (std::uint64_t page : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x7FFFFFFFFFFFFF},
                               std::uint64_t{0x80000000000000}}) {
        for (int slot : {0, 1, 255}) {
            const Price price = fastlob::tick_from_page(page, static_cast<std::uint8_t>(slot));
            EXPECT_EQ(tick_page_key(price), page);
            EXPECT_EQ(fastlob::tick_page_slot(price), slot);
        }
    }
}

TEST(PriceIndex, DirectedQuotesFifoAndPageEdges) {
    Books books(2048, 2048);
    std::vector<OrderId> live;
    OrderId next = 1;
    auto add = [&](Side side, Price price, Qty qty) {
        BookEvent event;
        event.type = EventType::Add;
        event.order_id = next++;
        event.side = side;
        event.price = price;
        event.qty = qty;
        ASSERT_TRUE(books.apply(event));
        live.push_back(event.order_id);
        ASSERT_TRUE(expect_match("add", books, live));
    };

    add(Side::Buy, -10, 5);
    add(Side::Buy, -1, 7);
    add(Side::Buy, 0, 9);
    add(Side::Buy, 255, 1);
    add(Side::Buy, 256, 2);
    add(Side::Buy, 100, 3);
    add(Side::Sell, -10, 4);
    add(Side::Sell, 255, 8);
    add(Side::Sell, 256, 6);
    add(Side::Sell, 0, 1);

    // Same-price FIFO, middle cancel, partial execute, qty decrease.
    add(Side::Buy, 100, 4);
    add(Side::Buy, 100, 5);
    const OrderId head = live[5];  // first buy @ 100, qty 3
    BookEvent execute;
    execute.type = EventType::Execute;
    execute.order_id = head;
    execute.qty = 1;
    ASSERT_TRUE(books.apply(execute));
    ASSERT_TRUE(expect_match("partial", books, live));

    BookEvent modify;
    modify.type = EventType::Modify;
    modify.order_id = live.back();
    modify.qty = 2;
    ASSERT_TRUE(books.apply(modify));
    BookEvent increase = modify;
    increase.qty = 9;
    EXPECT_FALSE(books.reference.apply(increase));
    EXPECT_FALSE(books.v1.apply(increase));
    EXPECT_FALSE(books.map_book.apply(increase));
    EXPECT_FALSE(books.paged.apply(increase));

    BookEvent cancel;
    cancel.type = EventType::Cancel;
    cancel.order_id = 11;  // middle of the 100-bid FIFO (qty 4, between id 6 and id 12)
    ASSERT_TRUE(books.apply(cancel));
    live.erase(std::remove(live.begin(), live.end(), cancel.order_id), live.end());
    ASSERT_TRUE(expect_match("middle cancel", books, live));

    // Drop the best bid and the edge between pages 0 and 1.
    for (OrderId id : {OrderId{6}, OrderId{4}, OrderId{5}}) {
        BookEvent drop;
        drop.type = EventType::Cancel;
        drop.order_id = id;
        if (books.reference.find(id)) {
            ASSERT_TRUE(books.apply(drop));
            live.erase(std::remove(live.begin(), live.end(), id), live.end());
            ASSERT_TRUE(expect_match("drop", books, live));
        }
    }
}

TEST(PriceIndex, FullPageAndNextPage) {
    constexpr int kPrices = 257;  // slots 0..255 plus the first tick of the next page
    Books books(static_cast<std::size_t>(kPrices + 8), static_cast<std::size_t>(kPrices + 8));
    std::vector<OrderId> live;
    for (int i = 0; i < kPrices; ++i) {
        BookEvent event;
        event.type = EventType::Add;
        event.order_id = static_cast<OrderId>(i + 1);
        event.side = Side::Sell;
        event.price = i;
        event.qty = static_cast<Qty>(1 + (i % 5));
        ASSERT_TRUE(books.apply(event));
        live.push_back(event.order_id);
    }
    ASSERT_TRUE(expect_match("full page", books, live));
    EXPECT_EQ(books.paged.page_stats().live_leaves, 2u);

    BookEvent drop;
    drop.type = EventType::Cancel;
    drop.order_id = 256;  // price 255, last slot of the first page
    ASSERT_TRUE(books.apply(drop));
    live.erase(std::remove(live.begin(), live.end(), drop.order_id), live.end());
    drop.order_id = 257;  // price 256, first slot of the next page
    ASSERT_TRUE(books.apply(drop));
    live.erase(std::remove(live.begin(), live.end(), drop.order_id), live.end());
    ASSERT_TRUE(expect_match("hole at page boundary", books, live));
}

TEST(PriceIndex, EmptyPageReuseAndRollback) {
    PagedStorageBook paged(8, 8);
    MapStorageBook map_book(8, 8);
    ReferenceOrderBook reference;

    auto apply_both = [&](BookEvent event) {
        const bool ok_ref = reference.apply(event);
        const bool ok_map = map_book.apply(event);
        const bool ok_paged = paged.apply(event);
        EXPECT_EQ(ok_ref, ok_map);
        EXPECT_EQ(ok_ref, ok_paged);
        return ok_ref;
    };

    BookEvent add;
    add.type = EventType::Add;
    add.order_id = 1;
    add.side = Side::Buy;
    add.price = -1;
    add.qty = 3;
    ASSERT_TRUE(apply_both(add));
    const auto after_hot = paged.page_stats();
    EXPECT_EQ(after_hot.live_leaves, 1u);

    BookEvent far;
    far.type = EventType::Add;
    far.order_id = 2;
    far.side = Side::Buy;
    far.price = -100000;
    far.qty = 1;
    ASSERT_TRUE(apply_both(far));
    const auto after_first = paged.page_stats();
    EXPECT_EQ(after_first.live_leaves, 2u);
    EXPECT_GT(after_first.committed_nodes, 2u);

    BookEvent cancel;
    cancel.type = EventType::Cancel;
    cancel.order_id = 2;
    ASSERT_TRUE(apply_both(cancel));
    const auto after_cancel = paged.page_stats();
    EXPECT_EQ(after_cancel.live_leaves, 1u);
    EXPECT_EQ(after_cancel.live_nodes, after_hot.live_nodes);
    EXPECT_EQ(after_cancel.committed_leaves, after_first.committed_leaves);
    EXPECT_EQ(after_cancel.committed_nodes, after_first.committed_nodes);

    far.order_id = 3;
    far.price = -200000;
    ASSERT_TRUE(apply_both(far));
    const auto after_reuse = paged.page_stats();
    EXPECT_EQ(after_reuse.live_leaves, 2u);
    EXPECT_EQ(after_reuse.committed_leaves, after_first.committed_leaves);
    EXPECT_EQ(after_reuse.committed_nodes, after_first.committed_nodes);
    EXPECT_EQ(reference.level_qty(Side::Buy, -200000), map_book.level_qty(Side::Buy, -200000));
    EXPECT_EQ(reference.level_qty(Side::Buy, -200000), paged.level_qty(Side::Buy, -200000));

    // Order pool is full: opening a second price must roll the new page back.
    PagedStorageBook tight(1, 8);
    ASSERT_TRUE(tight.add(1, Side::Buy, 0, 1));
    const auto before_fail = tight.page_stats();
    EXPECT_FALSE(tight.add(2, Side::Buy, 500000, 1));
    const auto after_fail = tight.page_stats();
    EXPECT_EQ(after_fail.live_leaves, before_fail.live_leaves);
    EXPECT_EQ(after_fail.live_nodes, before_fail.live_nodes);
    EXPECT_EQ(tight.order_count(), 1u);
    LevelView top[4];
    EXPECT_EQ(tight.top_n(Side::Buy, top), 1u);
    EXPECT_EQ(top[0].price, 0);

    MapStorageBook tight_map(1, 4);
    ASSERT_TRUE(tight_map.add(1, Side::Sell, 10, 2));
    EXPECT_FALSE(tight_map.add(2, Side::Sell, 11, 2));
    EXPECT_EQ(tight_map.active_levels(Side::Sell), 1u);
    EXPECT_EQ(tight_map.order_count(), 1u);

    PagedStorageBook level_cap(8, 2);
    ASSERT_TRUE(level_cap.add(1, Side::Buy, 1, 1));
    ASSERT_TRUE(level_cap.add(2, Side::Buy, 2, 1));
    EXPECT_FALSE(level_cap.add(3, Side::Buy, 3, 1));
    EXPECT_EQ(level_cap.active_levels(Side::Buy), 2u);
}

TEST(PriceIndex, Int64MinIsARealPriceOnTheNewBooks) {
    // v1's price hash uses INT64_MIN as the empty-slot sentinel, so it cannot
    // represent that tick. The map index and the paged directory can.
    const Price min_price = std::numeric_limits<Price>::min();
    ReferenceOrderBook reference;
    MapStorageBook map_book(16, 16);
    PagedStorageBook paged(16, 16);
    auto apply = [&](OrderId id, Side side, Price price, Qty qty) {
        BookEvent event;
        event.type = EventType::Add;
        event.order_id = id;
        event.side = side;
        event.price = price;
        event.qty = qty;
        EXPECT_TRUE(reference.apply(event));
        EXPECT_TRUE(map_book.apply(event));
        EXPECT_TRUE(paged.apply(event));
    };
    apply(1, Side::Buy, min_price, 4);
    apply(2, Side::Buy, min_price + 255, 5);
    apply(3, Side::Sell, min_price, 6);
    apply(4, Side::Sell, 0, 7);
    EXPECT_EQ(tick_page_key(min_price), tick_page_key(min_price + 255));
    EXPECT_EQ(paged.page_stats().live_leaves, 3u);

    LevelView bids[4];
    LevelView asks[4];
    const auto n_bid = reference.top_n(Side::Buy, bids);
    const auto n_ask = reference.top_n(Side::Sell, asks);
    LevelView map_bids[4];
    LevelView map_asks[4];
    LevelView paged_bids[4];
    LevelView paged_asks[4];
    EXPECT_EQ(map_book.top_n(Side::Buy, map_bids), n_bid);
    EXPECT_EQ(paged.top_n(Side::Buy, paged_bids), n_bid);
    EXPECT_EQ(map_book.top_n(Side::Sell, map_asks), n_ask);
    EXPECT_EQ(paged.top_n(Side::Sell, paged_asks), n_ask);
    for (std::size_t i = 0; i < n_bid; ++i) {
        EXPECT_EQ(map_bids[i].price, bids[i].price);
        EXPECT_EQ(paged_bids[i].price, bids[i].price);
        EXPECT_EQ(paged_bids[i].qty, bids[i].qty);
    }
    for (std::size_t i = 0; i < n_ask; ++i) {
        EXPECT_EQ(paged_asks[i].price, asks[i].price);
        EXPECT_EQ(paged_asks[i].qty, asks[i].qty);
    }
    EXPECT_EQ(paged_bids[0].price, min_price + 255);
    EXPECT_EQ(paged_asks[0].price, min_price);

    EXPECT_TRUE(reference.cancel(1));
    EXPECT_TRUE(map_book.cancel(1));
    EXPECT_TRUE(paged.cancel(1));
    EXPECT_EQ(paged.level_qty(Side::Buy, min_price + 255), 5u);
    EXPECT_EQ(paged.fifo_ids(Side::Buy, min_price + 255), reference.fifo_ids(Side::Buy, min_price + 255));
}

TEST(PriceIndex, RandomStreamMatchesIncludingEmptyPages) {
    constexpr std::uint64_t kSeed = 20261116;
    std::mt19937_64 rng(kSeed);
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<int> qty_dist(1, 12);
    std::uniform_int_distribution<int> price_dist(-40000, 40000);
    std::vector<Price> universe = {-40000, -1000, -256, -255, -1, 0, 1, 254, 255, 256, 257, 511, 512,
                                    1000, 5000, 100000, std::numeric_limits<Price>::max() - 10,
                                    std::numeric_limits<Price>::min() + 10,
                                    std::numeric_limits<Price>::min() + 255,
                                    std::numeric_limits<Price>::min() + 256};
    for (int i = 0; i < 40; ++i) {
        universe.push_back(price_dist(rng));
    }

    Books books(8192, 4096);
    struct Live {
        OrderId id;
        Side side;
        Price price;
        Qty qty;
    };
    std::vector<Live> live;
    std::vector<OrderId> ids;
    OrderId next = 1;

    for (int step = 0; step < 5000; ++step) {
        BookEvent event{};
        event.ts = static_cast<fastlob::Timestamp>(step + 1);
        const int roll = op_dist(rng);
        if (live.empty() || roll < 55) {
            event.type = EventType::Add;
            event.order_id = next++;
            event.side = (roll % 2 == 0) ? Side::Buy : Side::Sell;
            if (!live.empty() && roll % 3 == 0) {
                const Live& existing = live[static_cast<std::size_t>(rng() % live.size())];
                event.side = existing.side;
                event.price = existing.price;
            } else {
                event.price = universe[static_cast<std::size_t>(rng() % universe.size())];
            }
            event.qty = static_cast<Qty>(qty_dist(rng));
            if (!books.apply(event)) {
                ADD_FAILURE() << "add rejected at " << step;
                return;
            }
            live.push_back(Live{event.order_id, event.side, event.price, event.qty});
            ids.push_back(event.order_id);
        } else if (roll < 80) {
            const std::size_t index = static_cast<std::size_t>(rng() % live.size());
            event.type = EventType::Cancel;
            event.order_id = live[index].id;
            if (!books.apply(event)) {
                ADD_FAILURE() << "cancel rejected at " << step;
                return;
            }
            ids.erase(std::remove(ids.begin(), ids.end(), live[index].id), ids.end());
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            const std::size_t index = static_cast<std::size_t>(rng() % live.size());
            event.type = EventType::Execute;
            event.order_id = live[index].id;
            event.qty = static_cast<Qty>(1 + (rng() % live[index].qty));
            if (!books.apply(event)) {
                ADD_FAILURE() << "execute rejected at " << step;
                return;
            }
            live[index].qty -= event.qty;
            if (live[index].qty == 0) {
                ids.erase(std::remove(ids.begin(), ids.end(), live[index].id), ids.end());
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
        if (!expect_match("random", books, ids)) {
            ADD_FAILURE() << "seed " << kSeed << " step " << step;
            return;
        }
        std::vector<std::tuple<OrderId, Side, Price, Qty>> rows;
        rows.reserve(live.size());
        for (const Live& order : live) {
            rows.emplace_back(order.id, order.side, order.price, order.qty);
        }
        EXPECT_EQ(books.paged.page_stats().live_leaves, live_pages(rows)) << "step " << step;
    }

    for (const Live& order : live) {
        BookEvent cancel;
        cancel.type = EventType::Cancel;
        cancel.order_id = order.id;
        ASSERT_TRUE(books.apply(cancel));
    }
    EXPECT_EQ(books.paged.order_count(), 0u);
    EXPECT_EQ(books.paged.page_stats().live_leaves, 0u);
    EXPECT_EQ(books.paged.page_stats().live_nodes, 2u);
}

TEST(PriceIndex, DeepSlideMatches) {
    constexpr int kLevels = 600;
    constexpr int kSlides = 250;
    Books books(static_cast<std::size_t>(kLevels + kSlides + 8),
                static_cast<std::size_t>(kLevels + 8));
    std::vector<OrderId> live;
    for (int i = 0; i < kLevels; ++i) {
        BookEvent event;
        event.type = EventType::Add;
        event.order_id = static_cast<OrderId>(i + 1);
        event.side = Side::Sell;
        event.price = i;
        event.qty = 1;
        ASSERT_TRUE(books.apply(event));
        live.push_back(event.order_id);
    }
    ASSERT_TRUE(expect_match("deep setup", books, live));

    for (int step = 0; step < kSlides; ++step) {
        const Price leaving = step;
        BookEvent cancel;
        cancel.type = EventType::Cancel;
        cancel.order_id = static_cast<OrderId>(leaving + 1);
        ASSERT_TRUE(books.apply(cancel));
        live.erase(std::remove(live.begin(), live.end(), cancel.order_id), live.end());

        BookEvent add;
        add.type = EventType::Add;
        add.order_id = static_cast<OrderId>(kLevels + step + 1);
        add.side = Side::Sell;
        add.price = kLevels + step;
        add.qty = 1;
        ASSERT_TRUE(books.apply(add));
        live.push_back(add.order_id);
        if (!expect_match("slide", books, live)) {
            ADD_FAILURE() << "slide " << step;
            return;
        }
    }
}
