#include "fastlob/alloc_probe.hpp"
#include "fastlob/lob_fast.hpp"
#include "fastlob/storage_book.hpp"
#include "fastlob/synthetic_mbo.hpp"

#include <gtest/gtest.h>

using fastlob::FastOrderBook;
using fastlob::OrderId;
using fastlob::PagedLOB;
using fastlob::ScopedAllocProbe;
using fastlob::Side;

TEST(ZeroAlloc, AddCancelHotPath) {
#if !defined(FASTLOB_ENABLE_ALLOC_PROBE)
    GTEST_SKIP() << "built without FASTLOB_ENABLE_ALLOC_PROBE";
#endif
    FastOrderBook book(1 << 16, 1 << 12);

    // Warm: populate so indexes/levels already exist for reused prices.
    fastlob::SyntheticConfig cfg;
    cfg.num_events = 2000;
    cfg.seed = 1;
    auto warm = fastlob::generate_synthetic_mbo(cfg);
    for (const auto& e : warm) {
        book.apply(e);
    }

    const auto px = warm.front().price;
    // Ensure a known resting order we can cancel/re-add pattern on fixed prices.
    OrderId next = 9'000'000;
    ASSERT_TRUE(book.add(next, Side::Buy, px, 10));

    {
        ScopedAllocProbe probe;
        for (int i = 0; i < 1000; ++i) {
            const OrderId id = next + 1 + static_cast<OrderId>(i);
            ASSERT_TRUE(book.add(id, Side::Buy, px, 5));
            ASSERT_TRUE(book.cancel(id));
        }
        ASSERT_ZERO_ALLOC(probe);
    }
}

TEST(ZeroAlloc, PagedLobHotAndReservedColdPaths) {
#if !defined(FASTLOB_ENABLE_ALLOC_PROBE)
    GTEST_SKIP() << "built without FASTLOB_ENABLE_ALLOC_PROBE";
#endif
    PagedLOB book(1 << 14, 1 << 11);
    book.reserve_cold(32, 32);

    ASSERT_TRUE(book.add(1, Side::Sell, 10'000, 10));
    ASSERT_TRUE(book.add(2, Side::Buy, 9'990, 10));

    {
        ScopedAllocProbe probe;
        for (int i = 0; i < 500; ++i) {
            const OrderId id = 100 + static_cast<OrderId>(i * 2);
            // Hot-ring add + cancel + match_limit
            ASSERT_TRUE(book.add(id, Side::Sell, 10'005, 4));
            const auto m = book.match_limit(id + 1, Side::Buy, 10'005, 2);
            ASSERT_TRUE(m.ok);
            ASSERT_TRUE(book.cancel(id));
            // Cold-radix add + cancel within pre-reserved nodes/leaves
            const OrderId cold_id = 100'000 + static_cast<OrderId>(i);
            ASSERT_TRUE(book.add(cold_id, Side::Sell, 20'000 + (i % 512), 1));
            ASSERT_TRUE(book.cancel(cold_id));
        }
        ASSERT_ZERO_ALLOC(probe);
    }
}
