#include "fastlob/itch50.hpp"
#include "fastlob/lob_fast.hpp"
#include "fastlob/lob_reference.hpp"
#include "fastlob/storage_book.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace {

using fastlob::BookEvent;
using fastlob::FastOrderBook;
using fastlob::LevelView;
using fastlob::MapStorageBook;
using fastlob::OrderId;
using fastlob::PagedLOB;
using fastlob::ReferenceOrderBook;
using fastlob::Side;
namespace itch = fastlob::itch50;

TEST(Itch50, PartialCancelReplaceExecuteAndSymbolFilter) {
    std::vector<std::uint8_t> wire;
    itch::encode_system_event(wire, 'Q', 34'200'000'000'000ULL);
    itch::encode_stock_directory(wire, 13, "AAPL");
    itch::encode_stock_directory(wire, 5291, "MSFT");

    // Add two AAPL bids at $289.5000 (2,895,000 ticks) and one MSFT bid that must be ignored
    itch::encode_add_order(wire, 13, 100, 1001, Side::Buy, 500, "AAPL", 2'895'000);
    itch::encode_add_order(wire, 13, 101, 1002, Side::Buy, 300, "AAPL", 2'895'000);
    itch::encode_add_order(wire, 5291, 102, 9999, Side::Buy, 900, "MSFT", 1'580'000);

    // Partial cancel ('X') on 1001 by 200 shares -> 300 remaining, keeps FIFO head ahead of 1002
    itch::encode_order_cancel(wire, 13, 103, 1001, 200);
    // Execute ('E') 250 shares on 1001 -> 50 remaining on 1001
    itch::encode_order_executed(wire, 13, 104, 1001, 250);
    // Replace ('U') 1001 -> 1003 at $289.6000 for 400 shares (inherits Side::Buy!)
    itch::encode_order_replace(wire, 13, 105, 1001, 1003, 400, 2'896'000);
    // Delete ('D') 1002 completely
    itch::encode_order_delete(wire, 13, 106, 1002);
    itch::encode_system_event(wire, 'M', 57'600'000'000'000ULL);

    itch::ItchSymbolNormalizer norm("AAPL");
    PagedLOB paged(1024, 256);
    ReferenceOrderBook ref;

    const std::size_t msgs = itch::parse_itch50_buffer(
        wire.data(), wire.size(), [&](const std::uint8_t* msg, std::size_t len) {
            norm.on_message(msg, len, [&](const BookEvent& ev) {
                EXPECT_TRUE(paged.apply(ev));
                EXPECT_TRUE(ref.apply(ev));
            });
        });

    EXPECT_EQ(msgs, 11u);
    EXPECT_TRUE(norm.stats().locate_found);
    EXPECT_EQ(norm.stats().stock_locate, 13u);
    EXPECT_EQ(norm.stats().market_open_ts, 34'200'000'000'000ULL);
    EXPECT_EQ(norm.stats().market_close_ts, 57'600'000'000'000ULL);
    EXPECT_EQ(paged.order_count(), 1u);
    EXPECT_EQ(paged.best_price(Side::Buy), 28'960);
    EXPECT_EQ(paged.level_qty(Side::Buy, 28'960), 400u);
    LevelView ref_top[1]{};
    ASSERT_EQ(ref.top_n(Side::Buy, ref_top), 1u);
    EXPECT_EQ(paged.best_price(Side::Buy), ref_top[0].price);
    EXPECT_EQ(paged.order_count(), ref.order_count());
}

TEST(Itch50, WireFileStreamDifferentialAcrossAllBooks) {
    std::vector<std::uint8_t> wire;
    itch::encode_stock_directory(wire, 7451, "SPY");

    std::mt19937_64 rng(20261121ULL);
    std::vector<OrderId> live_ids;
    OrderId next_id = 1;
    for (int step = 0; step < 4000; ++step) {
        const std::uint32_t roll = static_cast<std::uint32_t>(rng() % 100);
        if (live_ids.empty() || roll < 50) {
            const OrderId id = next_id++;
            const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            const std::uint32_t base = (side == Side::Buy) ? 3'218'000u : 3'218'500u;
            const std::uint32_t px = base + static_cast<std::uint32_t>((rng() % 600) * 100);
            const std::uint32_t qty = static_cast<std::uint32_t>((rng() % 10 + 1) * 100);
            itch::encode_add_order(wire, 7451, static_cast<std::uint64_t>(step), id, side, qty, "SPY", px);
            live_ids.push_back(id);
        } else if (roll < 70) {
            const std::size_t idx = rng() % live_ids.size();
            itch::encode_order_cancel(wire, 7451, static_cast<std::uint64_t>(step), live_ids[idx], 50);
        } else if (roll < 85) {
            const std::size_t idx = rng() % live_ids.size();
            itch::encode_order_executed(wire, 7451, static_cast<std::uint64_t>(step), live_ids[idx], 100);
        } else if (roll < 93) {
            const std::size_t idx = rng() % live_ids.size();
            const OrderId old_id = live_ids[idx];
            const OrderId new_id = next_id++;
            const std::uint32_t px = 3'217'000u + static_cast<std::uint32_t>((rng() % 800) * 100);
            itch::encode_order_replace(wire, 7451, static_cast<std::uint64_t>(step), old_id, new_id, 300, px);
            live_ids[idx] = new_id;
        } else {
            const std::size_t idx = rng() % live_ids.size();
            itch::encode_order_delete(wire, 7451, static_cast<std::uint64_t>(step), live_ids[idx]);
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();
        }
    }

    const auto tmp = (std::filesystem::temp_directory_path() / "fastlob_itch50_test.itch").string();
    {
        std::ofstream out(tmp, std::ios::binary);
        ASSERT_TRUE(static_cast<bool>(out));
        out.write(reinterpret_cast<const char*>(wire.data()), static_cast<std::streamsize>(wire.size()));
    }

    itch::ItchSymbolNormalizer norm("SPY");
    PagedLOB paged(1 << 14, 1 << 12);
    MapStorageBook map_book(1 << 14, 1 << 12);
    FastOrderBook v1(1 << 14, 1 << 12);
    ReferenceOrderBook ref;

    ASSERT_TRUE(itch::stream_itch50_file(tmp, [&](const std::uint8_t* msg, std::size_t len) {
        norm.on_message(msg, len, [&](const BookEvent& ev) {
            (void)paged.apply(ev);
            (void)map_book.apply(ev);
            (void)v1.apply(ev);
            (void)ref.apply(ev);
        });
    }));

    std::filesystem::remove(tmp);

    EXPECT_EQ(paged.order_count(), ref.order_count());
    EXPECT_EQ(map_book.order_count(), ref.order_count());
    EXPECT_EQ(v1.order_count(), ref.order_count());
    EXPECT_EQ(paged.best_price(Side::Buy), map_book.best_price(Side::Buy));
    EXPECT_EQ(paged.best_price(Side::Sell), map_book.best_price(Side::Sell));

    for (Side side : {Side::Buy, Side::Sell}) {
        LevelView a[16]{};
        LevelView b[16]{};
        LevelView c[16]{};
        const std::size_t na = paged.top_n(side, a);
        const std::size_t nb = map_book.top_n(side, b);
        const std::size_t nc = ref.top_n(side, c);
        ASSERT_EQ(na, nb);
        ASSERT_EQ(na, nc);
        for (std::size_t i = 0; i < na; ++i) {
            EXPECT_EQ(a[i].price, c[i].price);
            EXPECT_EQ(a[i].qty, c[i].qty);
            EXPECT_EQ(b[i].price, c[i].price);
            EXPECT_EQ(b[i].qty, c[i].qty);
        }
    }
}

}  // namespace
