#include "fastlob/lob_fast.hpp"
#include "fastlob/lob_reference.hpp"
#include "fastlob/synthetic_mbo.hpp"

#include <gtest/gtest.h>

#include <array>

using fastlob::BookEvent;
using fastlob::EventType;
using fastlob::FastOrderBook;
using fastlob::LevelView;
using fastlob::ReferenceOrderBook;
using fastlob::Side;

namespace {

bool books_equal(const ReferenceOrderBook& ref, const FastOrderBook& fast) {
    if (ref.order_count() != fast.order_count()) {
        return false;
    }
    std::array<LevelView, 64> rb{};
    std::array<LevelView, 64> fb{};
    for (Side side : {Side::Buy, Side::Sell}) {
        const auto nr = ref.top_n(side, rb);
        const auto nf = fast.top_n(side, fb);
        if (nr != nf) {
            return false;
        }
        for (std::size_t i = 0; i < nr; ++i) {
            if (rb[i].price != fb[i].price || rb[i].qty != fb[i].qty) {
                return false;
            }
            auto rids = ref.fifo_ids(side, rb[i].price);
            auto fids = fast.fifo_ids(side, fb[i].price);
            if (rids != fids) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST(DifferentialLob, SyntheticLongStream) {
    fastlob::SyntheticConfig cfg;
    cfg.seed = 7;
    cfg.num_events = 50000;
    const auto events = fastlob::generate_synthetic_mbo(cfg);

    ReferenceOrderBook ref;
    FastOrderBook fast(1 << 18, 1 << 14);

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const BookEvent& e = events[i];
        const bool ok_r = ref.apply(e);
        const bool ok_f = fast.apply(e);
        if (ok_r != ok_f || !books_equal(ref, fast)) {
            ++mismatches;
            ADD_FAILURE() << "mismatch at event " << i << " type="
                          << static_cast<int>(e.type) << " id=" << e.order_id;
            break;
        }
    }
    EXPECT_EQ(mismatches, 0u);
    EXPECT_GT(ref.order_count(), 0u);
}
